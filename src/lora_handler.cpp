#include "lora_handler.h"
#include "config.h"
#include "settings.h"
#include <SPI.h>

// ––– Kodowanie protobuf (ręczne, bez zależności od nanopb) –––
// Format ramki Meshtastic/MeshCore na LoRa:
//   [1B LoRa header] [protobuf-encoded Data message]
// LoRa header: 0x30 | (hopLimit & 7), bit 0 = wantAck

static uint8_t* pb_write_varint(uint8_t* buf, uint32_t value) {
    while (value >= 0x80) {
        *buf++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *buf++ = (uint8_t)value;
    return buf;
}

static uint8_t* pb_write_tag(uint8_t* buf, uint8_t fieldNum, uint8_t wireType) {
    *buf++ = (fieldNum << 3) | wireType;
    return buf;
}

static uint8_t* pb_write_varint_field(uint8_t* buf, uint8_t fieldNum, uint32_t value) {
    buf = pb_write_tag(buf, fieldNum, 0);  // wire type 0 = varint
    return pb_write_varint(buf, value);
}

static uint8_t* pb_write_bytes_field(uint8_t* buf, uint8_t fieldNum, const uint8_t* data, uint8_t len) {
    buf = pb_write_tag(buf, fieldNum, 2);  // wire type 2 = length-delimited
    buf = pb_write_varint(buf, len);
    memcpy(buf, data, len);
    return buf + len;
}

// ––– Obiekty globalne –––
PacketRing<LoraPacket, 256> packetQueue;  // 256 ramek (~70 KB RAM)
volatile bool              packetReceived = false;
uint32_t                   packetCount    = 0;
uint32_t                   crcFailCount   = 0;
float                      lastRssi       = 0.0f;
float                      lastSnr        = 0.0f;
float                      lastFreqError  = 0.0f;
volatile float             currentRssi    = -200.0f;  // volatile dla współdzielenia ISR/loop
LoraPacket                 lastPacket;
ProtoType                  lastProto     = PROTO_UNKNOWN;
MeshCoreInfo               lastMcInfo;                        // ostatni zdekodowany nagłówek

// ––– Non-blocking LED blink state –––
static uint32_t ledBlinkUntil = 0;
static bool     ledBlinkActive = false;

void lora_led_blink_start(uint32_t duration_ms) {
    ledBlinkUntil = millis() + duration_ms;
    ledBlinkActive = true;
    digitalWrite(PIN_LED, HIGH);
}

void lora_led_blink_update() {
    if (ledBlinkActive && millis() >= ledBlinkUntil) {
        digitalWrite(PIN_LED, LOW);
        ledBlinkActive = false;
    }
}

bool lora_led_blink_active() {
    return ledBlinkActive;
}

// ––– Monitoring kanału –––
static RadioActivity activityState  = ACT_IDLE;
static uint32_t      activityUntil  = 0;
static uint32_t      lastRssiSample = 0;
static uint32_t      monitoringStart = 0;  // opóźniony start monitoringu po inicie
static bool          activityHistory = false;
static uint32_t      lastEnergySeen = 0;   // ostatni moment przekroczenia RSSI (do throttlingu loga)

// ––– Log zdarzeń –––
LogEntry     logBuffer[LOG_CAPACITY];
volatile int logHead  = -1;
int          logCount = 0;

// ––– Uproszczony log –––
SimpleLogEntry simpleLogBuffer[SIMPLE_LOG_CAPACITY];
volatile int   simpleLogHead  = -1;
int            simpleLogCount = 0;

// ––– Statystyki per-minute –––
uint16_t statsPerMinute[STATS_MINUTE_SIZE] = {0};
uint8_t  statsMinuteIndex = 0;
uint32_t statsCurrentMinuteStart = 0;

uint16_t statsPerHour[STATS_HOUR_SIZE] = {0};
uint8_t  statsHourIndex = 0;
static uint32_t statsCurrentHourStart = 0;
uint16_t statsLiveCounter = 0;  // bieżący licznik minutowy
uint16_t statsHourAccum    = 0;

void stats_tick() {
    uint32_t now = millis();
    if (statsCurrentMinuteStart == 0) {
        statsCurrentMinuteStart = now;
        statsCurrentHourStart   = now;
        return;
    }
    // Minute rotation
    if (now - statsCurrentMinuteStart >= 60000) {
        statsPerMinute[statsMinuteIndex] = statsLiveCounter;
        statsHourAccum += statsLiveCounter;
        statsMinuteIndex = (statsMinuteIndex + 1) % STATS_MINUTE_SIZE;
        statsPerMinute[statsMinuteIndex] = 0;
        statsLiveCounter = 0;
        statsCurrentMinuteStart = now;
    }
    // Hour rotation (3600s)
    if (now - statsCurrentHourStart >= 3600000) {
        statsPerHour[statsHourIndex] = statsHourAccum;
        statsHourIndex = (statsHourIndex + 1) % STATS_HOUR_SIZE;
        statsPerHour[statsHourIndex] = 0;
        statsHourAccum = 0;
        statsCurrentHourStart = now;
    }
}

void simple_log_add(char type, uint8_t routeType, uint8_t payloadType,
                    uint8_t hopCount, float rssi, float snr, const char* text) {
    int idx = (simpleLogHead + 1) % SIMPLE_LOG_CAPACITY;
    simpleLogBuffer[idx].timestamp   = millis();
    simpleLogBuffer[idx].type        = type;
    simpleLogBuffer[idx].routeType   = routeType;
    simpleLogBuffer[idx].payloadType = payloadType;
    simpleLogBuffer[idx].hopCount    = hopCount;
    simpleLogBuffer[idx].rssi        = rssi;
    simpleLogBuffer[idx].snr         = snr;
    if (text && text[0]) {
        strncpy(simpleLogBuffer[idx].text, text, 127);
        simpleLogBuffer[idx].text[127] = '\0';
    } else {
        simpleLogBuffer[idx].text[0] = '\0';
    }
    simpleLogHead = idx;
    if (simpleLogCount < SIMPLE_LOG_CAPACITY) simpleLogCount++;
    Serial.printf("[SLog] #%d %c rt=%d pt=%d hops=%d rssi=%.0f snr=%.1f txt='%s'\n",
                  simpleLogCount, type, routeType, payloadType, hopCount,
                  rssi, snr, simpleLogBuffer[idx].text);
}

static void log_add(char type, uint8_t len, float rssi, float snr,
                    const uint8_t* raw = nullptr, uint8_t rawLen = 0,
                    ProtoType proto = PROTO_UNKNOWN,
                    uint8_t mcRT = 0, uint8_t mcPT = 0, uint8_t mcPV = 0,
                    uint8_t mcHC = 0, uint8_t mcHS = 1, bool mcHT = false,
                    uint16_t mcT1 = 0, uint16_t mcT2 = 0) {
    int idx = (logHead + 1) % LOG_CAPACITY;
    logBuffer[idx].timestamp    = millis();
    logBuffer[idx].type         = type;
    logBuffer[idx].len          = len;
    logBuffer[idx].rssi         = rssi;
    logBuffer[idx].snr          = snr;
    logBuffer[idx].proto        = proto;
    logBuffer[idx].mcRouteType  = mcRT;
    logBuffer[idx].mcPayloadType = mcPT;
    logBuffer[idx].mcPayloadVer = mcPV;
    logBuffer[idx].mcHopCount   = mcHC;
    logBuffer[idx].mcHashSize   = mcHS;
    logBuffer[idx].mcHasTransport = mcHT;
    logBuffer[idx].mcTransport1 = mcT1;
    logBuffer[idx].mcTransport2 = mcT2;
    if (raw && rawLen > 0) {
        logBuffer[idx].dataLen = min(rawLen, (uint8_t)LOG_DATA_MAX);
        memcpy(logBuffer[idx].data, raw, logBuffer[idx].dataLen);
    } else {
        logBuffer[idx].dataLen = 0;
    }
    logHead = idx;
    if (logCount < LOG_CAPACITY) logCount++;
}

// ––– Identyfikacja protokołu –––
const char* proto_to_str(ProtoType p) {
    switch (p) {
        case PROTO_MESHCORE:   return "MeshCore";
        case PROTO_MESHTASTIC: return "Meshtastic";
        case PROTO_OTHER:      return "Other";
        default:               return "?";
    }
}

// ––– Nazwy pól MeshCore –––
const char* mc_route_type_name(uint8_t rt) {
    switch (rt) {
        case 0: return "TransportFlood";
        case 1: return "Flood";
        case 2: return "Direct";
        case 3: return "TransportDirect";
        default: return "?";
    }
}

const char* mc_payload_type_name(uint8_t pt) {
    switch (pt) {
        case 0x00: return "REQ";
        case 0x01: return "RESPONSE";
        case 0x02: return "TXT_MSG";
        case 0x03: return "ACK";
        case 0x04: return "ADVERT";
        case 0x05: return "GRP_TXT";
        case 0x06: return "GRP_DATA";
        case 0x07: return "ANON_REQ";
        case 0x08: return "PATH";
        case 0x09: return "TRACE";
        case 0x0A: return "MULTIPART";
        case 0x0B: return "CONTROL";
        case 0x0F: return "RAW_CUSTOM";
        default:   return "?";
    }
}

const char* mc_advert_type_name(uint8_t type) {
    switch (type) {
        case 0: return "NONE";
        case 1: return "CHAT";
        case 2: return "REPEATER";
        case 3: return "ROOM";
        case 4: return "SENSOR";
        default: return "?";
    }
}

// ––– Dekoder pakietu MeshCore (wg packet_format.md) –––
// Format: [header 1B] [transport_codes 4B?] [path_length 1B] [path N B] [payload]
// Header: 0bVVPPPPRR  (V=version, P=payloadType, R=routeType)
bool decode_meshcore(const uint8_t* data, uint8_t len, MeshCoreInfo& info) {
    if (!data || len < 2) return false;
    memset(&info, 0, sizeof(info));

    uint8_t hdr = data[0];
    info.routeType      = hdr & 0x03;
    info.payloadType    = (hdr >> 2) & 0x0F;
    info.payloadVersion = (hdr >> 6) & 0x03;

    uint8_t offset = 1;

    // Transport codes (tylko dla TRANSPORT_FLOOD i TRANSPORT_DIRECT)
    info.hasTransport = (info.routeType == 0 || info.routeType == 3);
    if (info.hasTransport) {
        if (offset + 4 > len) return false;
        info.transport1 = (uint16_t)data[offset] | ((uint16_t)data[offset+1] << 8);
        info.transport2 = (uint16_t)data[offset+2] | ((uint16_t)data[offset+3] << 8);
        offset += 4;
    }

    // Path length (koduje hop count + hash size)
    if (offset >= len) return false;
    uint8_t pl = data[offset];
    offset++;

    info.hopCount = pl & 0x3F;        // bity 0-5
    uint8_t hsCode = (pl >> 6) & 0x03; // bity 6-7 — hash size - 1
    info.hashSize = hsCode + 1;        // 1, 2, lub 3 bajty
    if (hsCode == 3) info.hashSize = 4; // 0b11 = 4 bajty (zarezerwowane)

    info.pathLen = info.hopCount * info.hashSize;
    if (info.pathLen > 64) return false; // MAX_PATH_SIZE

    if (offset + info.pathLen > len) return false;
    offset += info.pathLen;

    info.payloadOffset = offset;
    return true;
}

// ––– Priorytet ramki (do inteligentnego usuwania z kolejki) –––
// 0 = najniższy (sterujące/nieznane — usuwane jako pierwsze)
// 1 = średni   (REPEATER bez pozycji — usuwany gdy brak 0)
// 2 = wysoki   (pozycja lub tekst — zostaje jak najdłużej)
static int packet_priority(const LoraPacket& pkt) {
    ProtoType proto = classify_protocol(pkt.data, pkt.len);
    if (proto != PROTO_MESHCORE) return 0;  // nieznany protokół

    MeshCoreInfo mc;
    if (!decode_meshcore(pkt.data, pkt.len, mc)) return 0;

    // ADVERT (0x04)
    if (mc.payloadType == 0x04) {
        if (pkt.len > mc.payloadOffset + 100) {
            const uint8_t* ap = pkt.data + mc.payloadOffset + 100;
            uint8_t flags = ap[0];
            if (flags & 0x10) return 2;       // ma współrzędne → wysoki
            uint8_t advType = flags & 0x0F;
            if (advType == 2) return 1;       // REPEATER → średni (zostaje dopóki są ramki sterujące)
        }
        return 0;  // inny ADVERT bez pozycji → niski
    }

    // TXT_MSG (0x02): z tekstem → wysoki, pusty → niski
    if (mc.payloadType == 0x02) {
        return (pkt.len > mc.payloadOffset + 5) ? 2 : 0;
    }

    // ACK, REQ, PATH, TRACE, CONTROL... → niski (usuwane jako pierwsze)
    return 0;
}

// ––– Klasyfikator protokołu –––
// Próbuje zdekodować nagłówek MeshCore. Jeśli się uda → PROTO_MESHCORE.
// Jeśli nie → fallback do aktywnego profilu (sync word filtruje sprzętowo).
ProtoType classify_protocol(const uint8_t* data, uint8_t len) {
    if (!data || len < 2) return PROTO_UNKNOWN;

    ProtoType profileDefault = (g_settings.profile == PROFILE_MESHCORE)
                               ? PROTO_MESHCORE : PROTO_MESHTASTIC;

    // Próba dekodowania MeshCore
    MeshCoreInfo dummy;
    if (decode_meshcore(data, len, dummy)) {
        return PROTO_MESHCORE;
    }

    // Meshtastic MeshPacket: field 1 (from) fixed32 → tag 0x0D
    if (len >= 7 && data[1] == 0x0D) {
        return PROTO_MESHTASTIC;
    }

    // Data message: field 1 (portnum) varint → tag 0x08
    if (len >= 3 && data[1] == 0x08) {
        return PROTO_MESHCORE;  // stary format TX tego projektu
    }

    return profileDefault;
}

// ––– Odczyt 32-bit LE –––
static uint32_t read32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ––– Dekoder payloadu MeshCore –––
void decode_payload_summary(const uint8_t* data, uint8_t len,
                            const MeshCoreInfo& info, char* buf, size_t bufSize) {
    if (!data || !buf || bufSize == 0) return;
    buf[0] = '\0';
    if (len < info.payloadOffset) return;
    const uint8_t* p = data + info.payloadOffset;
    uint16_t remain = len - info.payloadOffset;

    switch (info.payloadType) {

    // --- ADVERT (0x04) ---
    case 0x04: {
        if (remain < 32) { snprintf(buf, bufSize, "ADVERT: too short (%uB)", remain); return; }
        // pubkey prefix (first 4 bytes)
        snprintf(buf, bufSize, "pub=0x%02X%02X%02X%02X",
                 p[0], p[1], p[2], p[3]);
        size_t pos = strlen(buf);
        // timestamp (offset 32, before signature)
        if (remain >= 36) {
            uint32_t ts = read32le(p + 32);
            if (ts > 1000000000) {
                snprintf(buf + pos, bufSize - pos, " ts=%u", ts);
                pos = strlen(buf);
            }
        }
        // appdata (after pubkey 32B + timestamp 4B + signature 64B = 100)
        if (remain > 100) {
            const uint8_t* ap = p + 100;
            uint16_t apLen = remain - 100;
            if (apLen > 0) {
                uint8_t flags = ap[0];
                uint8_t advType = flags & 0x0F;
                snprintf(buf + pos, bufSize - pos, " type=%s", mc_advert_type_name(advType));
                pos = strlen(buf);
                uint8_t off = 1;
                // lat/lon (bit 4)
                if ((flags & 0x10) && off + 8 <= apLen) {
                    int32_t lat = (int32_t)read32le(ap + off);
                    int32_t lon = (int32_t)read32le(ap + off + 4);
                    snprintf(buf + pos, bufSize - pos, " lat=%.5f lon=%.5f",
                             lat / 1000000.0, lon / 1000000.0);
                    pos = strlen(buf);
                    off += 8;
                }
                // feat1 (bit 5)
                if ((flags & 0x20) && off + 2 <= apLen) {
                    uint16_t f1 = (uint16_t)ap[off] | ((uint16_t)ap[off+1] << 8);
                    snprintf(buf + pos, bufSize - pos, " feat1=0x%04X", f1);
                    pos = strlen(buf);
                    off += 2;
                }
                // feat2 (bit 6)
                if ((flags & 0x40) && off + 2 <= apLen) {
                    uint16_t f2 = (uint16_t)ap[off] | ((uint16_t)ap[off+1] << 8);
                    snprintf(buf + pos, bufSize - pos, " feat2=0x%04X", f2);
                    pos = strlen(buf);
                    off += 2;
                }
                // name (bit 7)
                if ((flags & 0x80) && off < apLen) {
                    uint8_t nameLen = min((uint16_t)(apLen - off), (uint16_t)40);
                    snprintf(buf + pos, bufSize - pos, " name='%.*s'", nameLen, ap + off);
                }
            }
        }
        return;
    }

    // --- TXT_MSG (0x02) ---
    case 0x02: {
        if (remain < 2) { snprintf(buf, bufSize, "TXT: too short (%uB)", remain); return; }
        // Distinguish unencrypted vs encrypted by validating txtType/attempt
        // PLUS checking that text bytes look like real text (not random ciphertext).
        // Unencrypted: [timestamp 4B LE] [txtType:2b|attempt:2b 1B] [text N B]
        // Encrypted:   [dest 1B] [src 1B] [cipher MAC 2B] [ciphertext N B]
        uint8_t  tb = (remain >= 5) ? p[4] : 0xFF;
        uint8_t  txtType = tb >> 2;
        uint8_t  attempt = tb & 0x03;
        // Count printable ASCII bytes in first 8 chars of text (or full text if shorter)
        uint8_t textCheck  = (remain > 5) ? ((remain - 5) < 8 ? (uint8_t)(remain - 5) : 8) : 0;
        uint8_t printables = 0;
        for (uint8_t k = 0; k < textCheck; k++) {
            char c = (char)p[5 + k];
            if (c >= 0x20 && c < 0x7F) printables++;  // printable ASCII
            else if (c == '\n' || c == '\r' || c == '\t') printables++;  // whitespace
        }
        // Require valid txtType + ≥75% of first text bytes are printable
        bool looksPlain = (remain >= 6)
                       && (txtType <= 2)
                       && (attempt <= 3)
                       && (textCheck == 0 || printables * 4 >= textCheck * 3);  // ≥75%
        if (looksPlain) {
            uint32_t ts = read32le(p);
            const char* tname = (txtType == 0) ? "text" : (txtType == 1) ? "CLI" : "signed";
            snprintf(buf, bufSize, "%s #%u {%us} ", tname, attempt, ts);
            size_t pos = strlen(buf);
            uint16_t msgLen = remain - 5;
            if (msgLen > 0) {
                uint8_t show = min(msgLen, (uint16_t)(bufSize - pos - 4));
                for (uint8_t k = 0; k < show; k++) {
                    char c = (char)p[5 + k];
                    buf[pos + k] = (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                buf[pos + show] = '\0';
                if (msgLen > show) strcat(buf, "...");
            }
        } else {
            // Encrypted: [dest 1B] [src 1B] [cipher MAC 2B] [AES-256-GCM blob]
            // GCM blob = 12B nonce + ciphertext + 16B auth tag
            if (remain < 4) {
                snprintf(buf, bufSize, "dst=0x%02X src=0x%02X (too short)", p[0], p[1]);
            } else {
                uint16_t mac = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
                uint16_t blob = remain - 4;
                int16_t  ctext = (blob >= 28) ? (blob - 28) : blob;
                snprintf(buf, bufSize, "dst=0x%02X src=0x%02X mac=0x%04X GCM:%uB ctext",
                         p[0], p[1], mac, ctext);
            }
        }
        return;
    }

    // --- ACK (0x03) ---
    case 0x03: {
        if (remain < 4) { snprintf(buf, bufSize, "ACK: too short (%uB)", remain); return; }
        uint32_t chk = read32le(p);
        snprintf(buf, bufSize, "chk=0x%08X", chk);
        return;
    }

    // --- REQ (0x00), RESPONSE (0x01), PATH (0x08) ---
    // Protocol: [dest 1B] [src 1B] [cipher MAC 2B] [AES-256-GCM blob]
    case 0x00:
    case 0x01:
    case 0x08: {
        if (remain < 4) { snprintf(buf, bufSize, "%s: too short (%uB)",
                                     mc_payload_type_name(info.payloadType), remain); return; }
        uint16_t mac  = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
        uint16_t blob = remain - 4;
        int16_t  ctext = (blob >= 28) ? (blob - 28) : blob;
        snprintf(buf, bufSize, "dst=0x%02X src=0x%02X mac=0x%04X GCM:%uB ctext",
                 p[0], p[1], mac, ctext);
        return;
    }

    // --- ANON_REQ (0x07) ---
    // Protocol: [dest 1B] [pubkey 32B] [cipher MAC 2B] [AES-256-GCM blob]
    case 0x07: {
        if (remain < 36) { snprintf(buf, bufSize, "ANON_REQ: too short (%uB)", remain); return; }
        uint16_t mac  = (uint16_t)p[33] | ((uint16_t)p[34] << 8);
        uint16_t blob = remain - 35;
        int16_t  ctext = (blob >= 28) ? (blob - 28) : blob;
        snprintf(buf, bufSize, "dst=0x%02X pub=0x%02X%02X%02X%02X... mac=0x%04X GCM:%uB ctext",
                 p[0], p[1], p[2], p[3], p[4],
                 mac, ctext);
        return;
    }

    // --- GRP_TXT (0x05), GRP_DATA (0x06) ---
    // Protocol: [channelHash 1B] [cipher MAC 2B] [AES-256-GCM blob]
    case 0x05:
    case 0x06: {
        if (remain < 3) { snprintf(buf, bufSize, "%s: too short (%uB)",
                                     mc_payload_type_name(info.payloadType), remain); return; }
        uint16_t mac  = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
        uint16_t blob = remain - 3;
        int16_t  ctext = (blob >= 28) ? (blob - 28) : blob;
        snprintf(buf, bufSize, "ch=0x%02X mac=0x%04X GCM:%uB ctext",
                 p[0], mac, ctext);
        return;
    }

    // --- TRACE (0x09) ---
    case 0x09: {
        snprintf(buf, bufSize, "TRACE: %uB payload", remain);
        return;
    }

    // --- MULTIPART (0x0A) ---
    case 0x0A: {
        snprintf(buf, bufSize, "MULTIPART: %uB payload", remain);
        return;
    }

    // --- CONTROL (0x0B) ---
    // Protocol: [subType:4b | flags:4b 1B] [data N B]
    case 0x0B: {
        if (remain < 1) { snprintf(buf, bufSize, "CONTROL: empty"); return; }
        uint8_t subType = p[0] >> 4;
        uint8_t flags   = p[0] & 0x0F;
        const char* subName = "?";
        switch (subType) {
            case 0x8: subName = "DISCOVER_REQ"; break;
            case 0x9: subName = "DISCOVER_RESP"; break;
            default: break;
        }
        snprintf(buf, bufSize, "sub=%s(%u) flags=0x%X data=%uB",
                 subName, subType, flags, remain - 1);
        return;
    }

    // --- RAW_CUSTOM (0x0F) ---
    case 0x0F: {
        snprintf(buf, bufSize, "RAW_CUSTOM: %uB", remain);
        return;
    }

    // --- reserved / unknown ---
    default: {
        snprintf(buf, bufSize, "%uB payload", remain);
        return;
    }
    }
}

// ––– Sprzęt LoRa –––
// Jawnie tworzymy własną magistralę SPI, bo domyślne piny w definicji płytki
// mogą być błędne dla tego modułu.
static SPIClass    loraSpi(FSPI);
static SPISettings loraSpiSettings(1000000, MSBFIRST, SPI_MODE0);  // 1 MHz — max stabilne dla Heltec V3

// Moduł SX1262:  NSS, DIO1, RST, BUSY, SPI, SPI settings
static SX1262 radio = new Module(
    PIN_LORA_NSS,  PIN_LORA_DIO1,
    PIN_LORA_RST,  PIN_LORA_BUSY,
    loraSpi, loraSpiSettings);

// ––– ISR – wywoływana na zboczu DIO1 (odebrana ramka) –––
ICACHE_RAM_ATTR void onLoraPacket() {
    packetReceived = true;
}

// ––– Aktualne parametry pracy –––
static float   currentFreq = LORA_FALLBACK_FREQUENCY;
static uint8_t currentSF   = LORA_FALLBACK_SF;
static float   currentBW   = LORA_FALLBACK_BW;
static uint8_t currentCR   = LORA_FALLBACK_CR;
static bool    radioInited = false;   // czy radio było już inicjalizowane

// ––– Inicjalizacja LoRa –––
bool lora_init() {
    Serial.println(F("[LoRa] Inicjalizacja SX1262..."));

    // Wczytaj ustawienia z NVS (lub użyj domyślnych)
    settings_load();

    return lora_reinit();
}

bool lora_reinit() {
    // Hardware reset SX1262 — ensures clean state regardless of ESP32 reset type.
    // The radio is a separate chip; a warm ESP32 reset (RTS toggle after upload)
    // does NOT power-cycle it, so it may be stuck in TX, with a hung BUSY pin,
    // or in an otherwise broken SPI state.
    pinMode(PIN_LORA_RST, OUTPUT);
    digitalWrite(PIN_LORA_RST, LOW);
    delay(10);
    digitalWrite(PIN_LORA_RST, HIGH);
    delay(10);

    // Pobierz aktualne parametry z ustawień
    uint8_t syncWord;
    if (g_settings.profile == PROFILE_MESHCORE) {
        currentFreq = g_settings.frequency;
        currentSF   = MESHCORE_SF;
        currentBW   = MESHCORE_BW;
        currentCR   = MESHCORE_CR;
        syncWord    = LORA_SYNC_MESHCORE;
    } else {
        const PresetInfo& pres = preset_get_info(g_settings.preset);
        currentFreq = g_settings.frequency;
        currentSF   = pres.sf;
        currentBW   = pres.bw_khz;
        currentCR   = pres.cr;
        syncWord    = LORA_SYNC_MESHTASTIC;
    }

    // Zatrzymaj nasłuch i odłącz ISR (tylko jeśli radio było już uruchomione)
    if (radioInited) {
        radio.standby();
        radio.clearDio1Action();
        loraSpi.end();                 // zwolnij SPI przed ponowną inicjalizacją
    }

    // Inicjalizacja SPI (SS=-1 → ręczne zarządzanie NSS przez RadioLib)
    loraSpi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, -1);

    float tcxo = g_settings.tcxoVoltage;
    int state = radio.begin(
        currentFreq,
        currentBW,
        currentSF,
        currentCR,
        syncWord,
        LORA_TX_POWER,
        LORA_PREAMBLE_LEN,
        tcxo,
        LORA_USE_LDO);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd re-init, kod: %d\n", state);
        return false;
    }

    radio.setOutputPower(g_settings.txPower);
    radio.setDio1Action(onLoraPacket);

    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd startReceive, kod: %d\n", state);
        return false;
    }

    // Wyczyść kolejkę i flagi
    packetReceived  = false;
    packetQueue.clear();
    lora_reset_history();      // reset latcha przy zmianie konfiguracji
    monitoringStart = millis() + 3000;  // monitoring kanału dopiero za 3 s (stabilizacja radia)
    currentRssi     = radio.getRSSI(false);  // pierwszy odczyt RSSI (instant)

    radioInited = true;

    Serial.printf("[LoRa] Gotowy. %s | %.3f MHz | SF%d | BW %.0f kHz | CR 4/%d | Sync 0x%02X | TCXO %.1f V\n",
                  (g_settings.profile == PROFILE_MESHCORE) ? "MeshCore" : "Meshtastic",
                  currentFreq, currentSF, currentBW, currentCR, syncWord, tcxo);

    // ––– Diagnostyka: weryfikacja stanu radia po inicie –––
    {
        float rssiInst = radio.getRSSI(false);
        Serial.printf("[LoRa] Diag init: RSSI_inst=%.0f dBm (noise floor ~%.0f)\n",
                      rssiInst, rssiInst);
    }

    return true;
}

// ––– Przetwarzanie odebranych ramek – wołane z loop() –––
// WZORZEC: RadioLib community-verified — standby() PRZED readData() zapobiega
// lockupowi SPI (SX1262 po częściowym wykryciu preambuły potrafi zawiesić
// magistralę — objaw: RADIOLIB_ERR_SPI_CMD_FAILED = -2 przy kolejnych komendach).
void lora_process() {
    uint32_t now = millis();

    // Podczas rozgrzewania radia — ignoruj wszystko
    if (now < monitoringStart) {
        if (packetReceived) {
            packetReceived = false;
            int s = radio.startReceive();
            if (s != RADIOLIB_ERR_NONE) {
                Serial.printf("[LoRa] Błąd startReceive (warmup), kod: %d — próbuję reinit\n", s);
                lora_reinit();
            }
        }
        return;
    }

    // ––– Próbkowanie RSSI (do detekcji energii) –––
    if (now - lastRssiSample >= RSSI_SAMPLE_INTERVAL_MS) {
        lastRssiSample = now;
        currentRssi = radio.getRSSI(false);  // instant RSSI, nie z pakietu

        // Jeśli RSSI powyżej progu i nie ma aktywnego odbioru/CRC fail
        if (currentRssi > RSSI_ACTIVITY_THRESHOLD
            && activityState != ACT_RECEIVING
            && activityState != ACT_CRC_FAIL) {
            activityState = ACT_ENERGY;
            activityUntil  = now + ACTIVITY_HOLD_MS;
            activityHistory = true;
            // Loguj tylko jeśli przez ostatnie 10s NIE było przekroczenia
            if (lastEnergySeen == 0 || now - lastEnergySeen > 10000) {
                log_add('E', 0, currentRssi, 0);
            }
            lastEnergySeen = now;
        }

        // Zanik aktywności po czasie
        if (activityState == ACT_ENERGY && now > activityUntil) {
            activityState = ACT_IDLE;
        }
    }

    // ––– Diagnostyka: loguj noise floor co 10 s –––
    {
        static uint32_t lastDiagMs = 0;
        if (now - lastDiagMs >= 10000) {
            lastDiagMs = now;
            int dio1State = digitalRead(PIN_LORA_DIO1);
            Serial.printf("[LoRa] Diag: RSSI=%.0f dBm | DIO1=%d | pkts=%u | CRCfail=%u\n",
                          currentRssi, dio1State, packetCount, crcFailCount);
        }
    }

    if (!packetReceived) return;

    // ––– Critical section: prevent TOCTOU race on packetReceived flag –––
    // Detach ISR before clearing flag and reading data, reattach after startReceive()
    detachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1));
    packetReceived = false;

    int s = radio.standby();
    if (s != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd standby, kod: %d — próbuję reinit\n", s);
        lora_reinit();
        return;
    }

    LoraPacket pkt;

    // RadioLib v7: readData() takes len by VALUE — it is NOT updated.
    // Must call getPacketLength() BEFORE readData() to know the actual size.
    size_t actualLen = radio.getPacketLength();
    if (actualLen > sizeof(pkt.data)) {
        actualLen = sizeof(pkt.data);  // clamp to buffer size (safety)
    }

    int state = radio.readData(pkt.data, actualLen);
    pkt.len = (uint8_t)actualLen;

    if (state == RADIOLIB_ERR_NONE && pkt.len > 0) {
        // Metadane
        pkt.rssi       = radio.getRSSI();
        pkt.snr        = radio.getSNR();
        pkt.freqError  = radio.getFrequencyError();
        pkt.timestamp  = now;

        // Zapisz jako ostatni
        lastPacket    = pkt;
        lastRssi      = pkt.rssi;
        lastSnr       = pkt.snr;
        lastFreqError = pkt.freqError;
        packetCount++;
        statsLiveCounter++;  // per-minute stats
        currentRssi   = pkt.rssi;

        // Wrzuć do kolejki (z inteligentnym usuwaniem)
        // Priorytety: 0=sterujące/nieznane → 1=REPEATER → 2=pozycja/tekst
        bool queued = packetQueue.push(pkt);
        if (!queued) {
            for (int prio = 0; prio <= 1 && !queued; prio++) {
                for (size_t i = 0; i < packetQueue.count(); i++) {
                    LoraPacket existing;
                    if (packetQueue.peekAt(i, existing) && packet_priority(existing) == prio) {
                        packetQueue.removeAt(i);
                        Serial.printf("[LoRa] Evict prio=%d idx=%u\n", prio, i);
                        queued = packetQueue.push(pkt);
                        break;
                    }
                }
            }
            if (!queued) {
                // Wszystkie ramki prio 2 — evict najstarszej
                packetQueue.forcePush(pkt);
                Serial.println(F("[LoRa] Evict: wszystkie prio=2 — usunięto najstarszą"));
                queued = true;
            }
        }

        // Non-blocking LED blink (30ms)
        ledBlinkUntil = now + 30;
        ledBlinkActive = true;
        digitalWrite(PIN_LED, HIGH);

        // Sygnalizuj poprawny odbiór (trzymaj ~0.6s)
        activityState   = ACT_RECEIVING;
        activityUntil   = now + ACTIVITY_HOLD_MS;
        activityHistory = true;

        ProtoType proto = classify_protocol(pkt.data, pkt.len);
        lastProto = proto;

        // Dekoduj nagłówek MeshCore
        MeshCoreInfo mc;
        bool mcValid = (proto == PROTO_MESHCORE) && decode_meshcore(pkt.data, pkt.len, mc);
        if (mcValid) lastMcInfo = mc;

        log_add('R', pkt.len, pkt.rssi, pkt.snr, pkt.data, pkt.len, proto,
                mcValid ? mc.routeType : 0,
                mcValid ? mc.payloadType : 0,
                mcValid ? mc.payloadVersion : 0,
                mcValid ? mc.hopCount : 0,
                mcValid ? mc.hashSize : 1,
                mcValid ? mc.hasTransport : false,
                mcValid ? mc.transport1 : 0,
                mcValid ? mc.transport2 : 0);

        if (mcValid) {
            // Pełny dump MeshCore
            Serial.printf("[LoRa] RX #%u | MeshCore | RSSI: %.1f dBm | SNR: %.1f dB | len=%u\n",
                          packetCount, pkt.rssi, pkt.snr, pkt.len);
            Serial.printf("       Route=%s Payload=%s Ver=v%u Hops=%u",
                          mc_route_type_name(mc.routeType),
                          mc_payload_type_name(mc.payloadType),
                          mc.payloadVersion + 1,
                          mc.hopCount);
            if (mc.hasTransport) {
                Serial.printf(" TC1=0x%04X TC2=0x%04X", mc.transport1, mc.transport2);
            }
            Serial.printf(" PathLen=%u\n", mc.pathLen);
            // Hex dump of path hashes (one per hop)
            if (mc.hopCount > 0 && mc.pathLen > 0) {
                Serial.print(F("       Path: "));
                for (uint8_t h = 0; h < mc.hopCount && h * mc.hashSize < mc.pathLen; h++) {
                    if (h > 0) Serial.print(' ');
                    for (uint8_t b = 0; b < mc.hashSize; b++)
                        Serial.printf("%02X", pkt.data[mc.payloadOffset - mc.pathLen + h * mc.hashSize + b]);
                }
                Serial.println();
            }
            // Dekoduj payload
            char payloadBuf[128];
            decode_payload_summary(pkt.data, pkt.len, mc, payloadBuf, sizeof(payloadBuf));
            Serial.printf("       Payload: %s\n", payloadBuf);
            // Uproszczony log
            simple_log_add('R', mc.routeType, mc.payloadType, mc.hopCount,
                          pkt.rssi, pkt.snr, payloadBuf);
        } else {
            Serial.printf("[LoRa] RX #%u | %s | RSSI: %.1f dBm | SNR: %.1f dB | len=%u\n",
                          packetCount, proto_to_str(proto), pkt.rssi, pkt.snr, pkt.len);
            // Uproszczony log — non-MeshCore (routeType/payloadType = 0xFF)
            simple_log_add('R', 0xFF, 0xFF, 0, pkt.rssi, pkt.snr, proto_to_str(proto));
        }

        // Hex dump w monitorze szeregowym (pełna ramka)
        Serial.print(F("       "));
        for (uint8_t i = 0; i < pkt.len; i++) {
            Serial.printf("%02X ", pkt.data[i]);
        }
        Serial.println();
    }
    else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        crcFailCount++;
        float snr      = radio.getSNR();
        float freqErr  = radio.getFrequencyError();
        activityState  = ACT_CRC_FAIL;
        activityUntil  = now + ACTIVITY_HOLD_MS;
        activityHistory = true;
        log_add('C', 0, currentRssi, snr);

        Serial.printf("[LoRa] CRC FAIL #%u | RSSI:%.1f dBm | SNR:%.1f | FreqErr:%.1f Hz\n",
                      crcFailCount, currentRssi, snr, freqErr);
    }
    else if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd odczytu ramki, kod: %d\n", state);
    }

    // Wznów nasłuch z obsługą błędu
    s = radio.startReceive();
    if (s != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd startReceive po RX, kod: %d — próbuję reinit\n", s);
        lora_reinit();
    }

    // Re-enable ISR after radio is back in RX mode
    attachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1), onLoraPacket, RISING);
}

float lora_get_frequency_error() {
    return lastFreqError;
}

float   lora_get_frequency()  { return currentFreq; }
uint8_t lora_get_sf()         { return currentSF; }
float   lora_get_bandwidth()  { return currentBW; }
uint8_t lora_get_cr()         { return currentCR; }

RadioActivity lora_get_activity() {
    uint32_t now = millis();

    // Zanik stanów po czasie hold
    if ((activityState == ACT_RECEIVING || activityState == ACT_CRC_FAIL || activityState == ACT_ENERGY)
        && now > activityUntil) {
        activityState = ACT_IDLE;
    }

    return activityState;
}

bool lora_has_history() {
    return activityHistory;
}

bool lora_has_rx() {
    if (packetCount == 0) return false;
    return (millis() - lastPacket.timestamp) < 2000;
}

void lora_reset_history() {
    activityHistory  = false;
    activityState    = ACT_IDLE;
    activityUntil    = 0;
    monitoringStart  = millis() + 1000;  // 1s wyciszenia po resecie
    lastEnergySeen   = 0;
}

// ––– TX: nadawanie ramki –––
// MeshCore (wg packet_format.md):
//   [header 1B] [transport_codes 4B?] [path_length 1B] [path N B] [payload]
//   Header: 0bVVPPPPRR  (V=version, P=payloadType, R=routeType)
//   Dla TXT_MSG/Flood: [0x09] [0x00] [payload]   (tylko 2 B overhead)
//
// Meshtastic (stary format protobuf — do usunięcia po migracji):
//   [1B LoRa header 0x33] [Data protobuf]
//
static uint32_t txPacketId = 0;

// Zbuduj ramkę MeshCore
static uint8_t build_meshcore_frame(uint8_t* buf, const uint8_t* payload, uint8_t payloadLen) {
    // Header: v1, TXT_MSG, Flood
    uint8_t hdr = (0 << 6)     // version = 0 (v1)
                | (0x02 << 2)  // payloadType = TXT_MSG
                | (1);         // routeType = Flood
    buf[0] = hdr;
    buf[1] = 0x00;  // path_length: 0 hops, 1-byte hash

    // TXT_MSG payload header: timestamp(4B LE) + txt_type/attempt(1B)
    uint32_t ts = millis() / 1000;  // pseudo-unix-timestamp
    buf[2] = ts & 0xFF;
    buf[3] = (ts >> 8) & 0xFF;
    buf[4] = (ts >> 16) & 0xFF;
    buf[5] = (ts >> 24) & 0xFF;
    buf[6] = 0x00;  // txt_type=0 (plain text), attempt=0

    if (payloadLen > 0) {
        memcpy(buf + 7, payload, payloadLen);
    }
    return 7 + payloadLen;  // 2 (header+path) + 5 (TXT header) + text
}

// Zbuduj ramkę Meshtastic (stary format protobuf)
static uint8_t build_meshtastic_frame(uint8_t* buf, const uint8_t* payload, uint8_t payloadLen) {
    uint8_t* p = buf;
    *p++ = 0x33;  // LoRa header
    p = pb_write_varint_field(p, 1, 1);              // portnum=TEXT_MESSAGE_APP
    p = pb_write_bytes_field(p, 2, payload, payloadLen); // payload
    p = pb_write_varint_field(p, 4, 0xFFFFFFFF);     // dest=broadcast
    p = pb_write_varint_field(p, 5, g_settings.nodeId);  // sender
    p = pb_write_varint_field(p, 6, txPacketId++);   // packet_id
    p = pb_write_varint_field(p, 11, 3);             // hop_limit
    return p - buf;
}

bool lora_tx(const uint8_t* payload, uint8_t payloadLen) {
    // MAX_PACKET_PAYLOAD = 184, TXT_MSG header = 5 B → max text = 179
    if (payloadLen > 179) return false;

    // LED — sygnalizacja nadawania
    digitalWrite(PIN_LED, HIGH);
    lora_led_blink_start(200);

    // Zatrzymaj nasłuch
    int s = radio.standby();
    if (s != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd standby przed TX, kod: %d — próbuję reinit\n", s);
        digitalWrite(PIN_LED, LOW);
        lora_reinit();
        return false;
    }
    radio.clearDio1Action();
    packetReceived = false;

    // Ustaw moc TX z ustawień
    radio.setOutputPower(g_settings.txPower);

    // ––– Zbuduj ramkę –––
    uint8_t buf[256];
    uint8_t totalLen;
    ProtoType txProto;
    MeshCoreInfo mcTx;

    if (g_settings.profile == PROFILE_MESHCORE) {
        totalLen = build_meshcore_frame(buf, payload, payloadLen);
        txProto = PROTO_MESHCORE;
        // Wypełnij info dla logu
        memset(&mcTx, 0, sizeof(mcTx));
        mcTx.routeType = 1;  // Flood
        mcTx.payloadType = 0x02;  // TXT_MSG
        mcTx.payloadVersion = 0;  // v1
        mcTx.hopCount = 0;
        mcTx.hashSize = 1;
        mcTx.hasTransport = false;
        mcTx.payloadOffset = 7;  // header(1) + path_length(1) + TXT header(5)
        mcTx.pathLen = 0;
    } else {
        totalLen = build_meshtastic_frame(buf, payload, payloadLen);
        txProto = PROTO_MESHTASTIC;
    }

    Serial.printf("[LoRa] TX #%u | %s | %u B payload | frame=%u B | %d dBm\n",
                  txPacketId++, proto_to_str(txProto), payloadLen, totalLen, g_settings.txPower);
    if (txProto == PROTO_MESHCORE) {
        Serial.printf("       Header=0x%02X  Route=%s  Payload=%s  v%u  Hops=%u\n",
                      buf[0],
                      mc_route_type_name(mcTx.routeType),
                      mc_payload_type_name(mcTx.payloadType),
                      mcTx.payloadVersion + 1,
                      mcTx.hopCount);
    }

    int state = radio.transmit(buf, totalLen);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] TX error, code: %d\n", state);
        radio.setDio1Action(onLoraPacket);
        s = radio.startReceive();
        if (s != RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa] Błąd startReceive po błędzie TX, kod: %d — reinit\n", s);
            lora_reinit();
        }
        return false;
    }

    log_add('T', totalLen, 0.0f, 0, buf, min((uint8_t)LOG_DATA_MAX, totalLen), txProto,
            mcTx.routeType, mcTx.payloadType, mcTx.payloadVersion,
            mcTx.hopCount, mcTx.hashSize, mcTx.hasTransport,
            mcTx.transport1, mcTx.transport2);

    // Uproszczony log TX
    {
        char txText[128];
        if (payloadLen > 0) {
            uint8_t copyLen = min(payloadLen, (uint8_t)127);
            memcpy(txText, payload, copyLen);
            txText[copyLen] = '\0';
        } else {
            txText[0] = '\0';
        }
        uint8_t txRt = (txProto == PROTO_MESHCORE) ? mcTx.routeType : (uint8_t)0xFF;
        uint8_t txPt = (txProto == PROTO_MESHCORE) ? mcTx.payloadType : (uint8_t)0xFF;
        simple_log_add('T', txRt, txPt, mcTx.hopCount, 0.0f, 0.0f, txText);
    }

    // Wróć do nasłuchu
    radio.setDio1Action(onLoraPacket);
    int rxState = radio.startReceive();
    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd startReceive po TX, kod: %d — próbuję reinit\n", rxState);
        lora_reinit();
    }

    return true;
}
