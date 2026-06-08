#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// ––– Struktura odebranej ramki LoRa –––
struct LoraPacket {
    uint8_t  data[256];
    uint8_t  len;
    float    rssi;
    float    snr;
    float    freqError;
    uint32_t timestamp;  // millis() w momencie odbioru
};

// ––– Prosty bufor cykliczny (single-thread, tylko main loop) –––
template <typename T, size_t CAPACITY>
class PacketRing {
    T      _buf[CAPACITY];
    size_t _head = 0;
    size_t _count = 0;

public:
    bool push(const T& item) {
        if (_count >= CAPACITY) return false;
        size_t idx = (_head + _count) % CAPACITY;
        _buf[idx] = item;
        _count++;
        return true;
    }

    bool pop(T& item) {
        if (_count == 0) return false;
        item = _buf[_head];
        _head = (_head + 1) % CAPACITY;
        _count--;
        return true;
    }

    bool peek(T& item) const {
        if (_count == 0) return false;
        item = _buf[_head];
        return true;
    }

    size_t count() const { return _count; }
    bool   isEmpty() const { return _count == 0; }
    void   clear() { _head = _count = 0; }
};

// ––– Stan globalny –––
extern PacketRing<LoraPacket, 64> packetQueue;  // zwiększono z 16 na 64
extern volatile bool               packetReceived;
extern uint32_t                    packetCount;
extern uint32_t                    crcFailCount;
extern float                       lastRssi;
extern float                       lastSnr;
extern float                       lastFreqError;
extern volatile float              currentRssi;      // bieżący RSSI (próbkowany) - volatile dla współdzielenia ISR/loop
extern LoraPacket                  lastPacket;

// ––– Typy aktywności na kanale –––
enum RadioActivity : uint8_t {
    ACT_IDLE = 0,       // cisza – RSSI poniżej progu
    ACT_ENERGY,         // wykryto energię (RSSI > próg), brak preambuły
    ACT_RECEIVING,      // poprawny odbiór ramki (CRC OK)
    ACT_CRC_FAIL        // ramka odebrana ale CRC błędne
};

// ––– API –––
bool  lora_init();
bool  lora_reinit();     // restart radia z nowymi ustawieniami
void  lora_process();
bool  lora_tx(const uint8_t* data, uint8_t len);  // nadaj ramkę
float lora_get_frequency_error();

// ––– Ekspozycja aktualnych parametrów (do wyświetlacza/API) –––
float         lora_get_frequency();
uint8_t       lora_get_sf();
float         lora_get_bandwidth();
uint8_t       lora_get_cr();
RadioActivity lora_get_activity();   // bieżący stan aktywności kanału
bool          lora_has_history();    // true jeśli kiedykolwiek wykryto aktywność
bool          lora_has_rx();         // true jeśli odebrano poprawną ramkę w ostatnich 2s
void          lora_reset_history();  // reset latcha (wołane przy zmianie konfiguracji)

// ––– LED blink control (non-blocking) –––
void          lora_led_blink_start(uint32_t duration_ms = 30);
void          lora_led_blink_update();
bool          lora_led_blink_active();

// ––– Identyfikacja protokołu –––
enum ProtoType : uint8_t {
    PROTO_UNKNOWN   = 0,   // nie udało się sklasyfikować
    PROTO_MESHCORE  = 1,   // MeshCore
    PROTO_MESHTASTIC = 2,  // Meshtastic — MeshPacket z fixed32
    PROTO_OTHER     = 3    // inny protokół (RAW, własny, itp.)
};

const char* proto_to_str(ProtoType p);

// ––– MeshCore — nazwy pól –––
const char* mc_route_type_name(uint8_t rt);
const char* mc_payload_type_name(uint8_t pt);

// ––– Zdekodowany nagłówek MeshCore –––
struct MeshCoreInfo {
    uint8_t  routeType;       // 0-3
    uint8_t  payloadType;     // 0-15
    uint8_t  payloadVersion;  // 0-3
    uint8_t  hopCount;        // 0-63
    uint8_t  hashSize;        // 1, 2, lub 3 bajty na hash
    bool     hasTransport;    // transport codes present
    uint16_t transport1;      // transport code 1 (jeśli hasTransport)
    uint16_t transport2;      // transport code 2 (jeśli hasTransport)
    uint8_t  payloadOffset;   // offset do payloadu
    uint8_t  pathLen;         // długość ścieżki w bajtach
};

bool decode_meshcore(const uint8_t* data, uint8_t len, MeshCoreInfo& info);

// ––– Dekoder payloadu MeshCore –––
// Zwraca tekstowy opis payloadu (max 128 znaków).
// Wywołujący zapewnia buf o rozmiarze >= 128.
void decode_payload_summary(const uint8_t* data, uint8_t len,
                            const MeshCoreInfo& info, char* buf, size_t bufSize);

// ––– Log zdarzeń –––
#define LOG_CAPACITY 64
#define LOG_DATA_MAX 255      // max bajtów do podglądu w logu
struct LogEntry {
    uint32_t timestamp;       // millis() w momencie zdarzenia
    char     type;            // 'R'=RX OK, 'C'=CRC fail, 'E'=Energy, 'T'=TX
    uint8_t  len;             // długość ramki (0 dla energy)
    float    rssi;
    float    snr;
    uint8_t  dataLen;         // ile bajtów w data[]
    uint8_t  data[LOG_DATA_MAX];
    ProtoType proto;          // typ protokołu (tylko dla 'R' i 'T')
    // ––– Pola MeshCore (ważne tylko gdy proto == PROTO_MESHCORE) –––
    uint8_t  mcRouteType;     // 0-3
    uint8_t  mcPayloadType;   // 0-15
    uint8_t  mcPayloadVer;    // 0-3
    uint8_t  mcHopCount;      // 0-63
    uint8_t  mcHashSize;      // 1, 2, lub 3 bajty
    bool     mcHasTransport;  // transport codes present
    uint16_t mcTransport1;    // transport code 1
    uint16_t mcTransport2;    // transport code 2
};

extern LogEntry      logBuffer[LOG_CAPACITY];
extern volatile int  logHead;   // index najnowszego wpisu
extern int           logCount;  // liczba wpisów (max LOG_CAPACITY)
extern ProtoType     lastProto; // protokół ostatniej ramki
extern MeshCoreInfo  lastMcInfo; // zdekodowany nagłówek MeshCore

// ––– Uproszczony log (tylko TX/RX, route, payload, hops, text) –––
#define SIMPLE_LOG_CAPACITY 64
struct SimpleLogEntry {
    uint32_t timestamp;       // millis() w momencie zdarzenia
    char     type;            // 'R'=RX, 'T'=TX
    uint8_t  routeType;       // 0-3 (MeshCore) lub 0xFF dla non-MeshCore
    uint8_t  payloadType;     // 0-15 (MeshCore) lub 0xFF
    uint8_t  hopCount;        // 0-63
    float    rssi;
    float    snr;
    char     text[128];       // zdekodowany payload (pusty string jeśli N/A)
};

extern SimpleLogEntry simpleLogBuffer[SIMPLE_LOG_CAPACITY];
extern volatile int   simpleLogHead;
extern int            simpleLogCount;

void simple_log_add(char type, uint8_t routeType, uint8_t payloadType,
                    uint8_t hopCount, float rssi, float snr, const char* text);

// ––– Klasyfikator ramek –––
ProtoType classify_protocol(const uint8_t* data, uint8_t len);
