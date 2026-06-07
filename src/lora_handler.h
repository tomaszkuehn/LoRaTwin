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
    PROTO_MESHCORE  = 1,   // MeshCore — Data z varintami
    PROTO_MESHTASTIC = 2,  // Meshtastic — MeshPacket z fixed32
    PROTO_OTHER     = 3    // inny protokół (RAW, własny, itp.)
};

const char* proto_to_str(ProtoType p);

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
};

extern LogEntry      logBuffer[LOG_CAPACITY];
extern volatile int  logHead;   // index najnowszego wpisu
extern int           logCount;  // liczba wpisów (max LOG_CAPACITY)
extern ProtoType     lastProto; // protokół ostatniej ramki

// ––– Klasyfikator ramek –––
ProtoType classify_protocol(const uint8_t* data, uint8_t len);
