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
PacketRing<LoraPacket, 64> packetQueue;  // zwiększono z 16 na 64
volatile bool              packetReceived = false;
uint32_t                   packetCount    = 0;
uint32_t                   crcFailCount   = 0;
float                      lastRssi       = 0.0f;
float                      lastSnr        = 0.0f;
float                      lastFreqError  = 0.0f;
volatile float             currentRssi    = -200.0f;  // volatile dla współdzielenia ISR/loop
LoraPacket                 lastPacket;

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

static void log_add(char type, uint8_t len, float rssi, float snr,
                    const uint8_t* raw = nullptr, uint8_t rawLen = 0) {
    int idx = (logHead + 1) % LOG_CAPACITY;
    logBuffer[idx].timestamp = millis();
    logBuffer[idx].type      = type;
    logBuffer[idx].len       = len;
    logBuffer[idx].rssi      = rssi;
    logBuffer[idx].snr       = snr;
    if (raw && rawLen > 0) {
        logBuffer[idx].dataLen = min(rawLen, (uint8_t)LOG_DATA_MAX);
        memcpy(logBuffer[idx].data, raw, logBuffer[idx].dataLen);
    } else {
        logBuffer[idx].dataLen = 0;
    }
    logHead = idx;
    if (logCount < LOG_CAPACITY) logCount++;
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
        currentRssi   = pkt.rssi;

        // Wrzuć do kolejki
        packetQueue.push(pkt);

        // Non-blocking LED blink (30ms)
        ledBlinkUntil = now + 30;
        ledBlinkActive = true;
        digitalWrite(PIN_LED, HIGH);

        // Sygnalizuj poprawny odbiór (trzymaj ~0.6s)
        activityState   = ACT_RECEIVING;
        activityUntil   = now + ACTIVITY_HOLD_MS;
        activityHistory = true;
        log_add('R', pkt.len, pkt.rssi, pkt.snr, pkt.data, pkt.len);

        Serial.printf("[LoRa] RX #%u | RSSI: %.1f dBm | SNR: %.1f dB | len=%u\n",
                      packetCount, pkt.rssi, pkt.snr, pkt.len);

        // Hex dump w monitorze szeregowym (tylko pierwsze 32 bajty)
        Serial.print(F("       "));
        for (uint8_t i = 0; i < min(pkt.len, (uint8_t)32); i++) {
            Serial.printf("%02X ", pkt.data[i]);
        }
        if (pkt.len > 32) Serial.print(F("..."));
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

// ––– TX: nadawanie ramki w formacie Meshtastic / MeshCore –––
// Format ramki: [1B LoRa header] [protobuf-encoded Data message]
// Data message fields: portnum(1), payload(2), dest(4), sender(5), packet_id(6), hop_limit(11)
static uint32_t txPacketId = 0;

bool lora_tx(const uint8_t* payload, uint8_t payloadLen) {
    if (payloadLen > 200) return false;  // maks ~200 B payloadu + protobuf overhead

    // LED — sygnalizacja nadawania (włącz przed TX, timer zgasi automatycznie)
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

    // ––– Zbuduj ramkę Meshtastic/MeshCore –––
    uint8_t buf[256];
    uint8_t* p = buf;

    // 1. LoRa header byte: hopStart=3, hopLimit=3, wantAck=0
    //    Bits: [7:4]=hopStart, [3]=reserved, [2:1]=hopLimit, [0]=wantAck
    //    0x30 | 0x03 = 0x33
    *p++ = 0x33;

    // 2. Protobuf Data message
    //
    //    Field 1 (portnum): varint → tag 0x08, wire_type 0
    //    PortNum.TEXT_MESSAGE_APP = 1
    p = pb_write_varint_field(p, 1, 1);

    //    Field 2 (payload): length-delimited → tag 0x12, wire_type 2
    p = pb_write_bytes_field(p, 2, payload, payloadLen);

    //    Field 4 (dest): varint → tag 0x20, wire_type 0
    //    Broadcast = 0xFFFFFFFF
    p = pb_write_varint_field(p, 4, 0xFFFFFFFF);

    //    Field 5 (sender): varint → tag 0x28, wire_type 0
    p = pb_write_varint_field(p, 5, g_settings.nodeId);

    //    Field 6 (packet_id): varint → tag 0x30, wire_type 0
    p = pb_write_varint_field(p, 6, txPacketId++);

    //    Field 11 (hop_limit): varint → tag 0x58, wire_type 0
    p = pb_write_varint_field(p, 11, 3);

    uint8_t totalLen = p - buf;

    Serial.printf("[LoRa] TX #%u | %u B (MeshCore proto) | %d dBm | timeout ~%lu ms\n",
                  txPacketId - 1, totalLen, g_settings.txPower,
                  5 + (radio.getTimeOnAir(totalLen) * 5) / 1000);

    int state = radio.transmit(buf, totalLen);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] TX error, code: %d\n", state);
        // Próba powrotu do RX mimo błędu
        radio.setDio1Action(onLoraPacket);
        s = radio.startReceive();
        if (s != RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa] Błąd startReceive po błędzie TX, kod: %d — reinit\n", s);
            lora_reinit();
        }
        return false;
    }

    log_add('T', totalLen, g_settings.txPower, 0, buf, min((uint8_t)LOG_DATA_MAX, totalLen));

    // Wróć do nasłuchu
    radio.setDio1Action(onLoraPacket);
    int rxState = radio.startReceive();
    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Błąd startReceive po TX, kod: %d — próbuję reinit\n", rxState);
        lora_reinit();
    }

    return true;
}
