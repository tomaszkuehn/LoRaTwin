#pragma once
#include <Arduino.h>

// ––– WiFi — domyślne dane (fallback, nadpisywane przez NVS / web UI) –––
#define WIFI_SSID "LoRaTwin"
#define WIFI_PASS "1234"

// ––– LoRa – stałe sprzętowe –––
#define LORA_SYNC_MESHTASTIC    0x2B
#define LORA_SYNC_MESHCORE      0x12
#define LORA_PREAMBLE_LEN       16
#define LORA_TX_POWER           10       // dBm – moc nadawania (fallback)

// ––– SX1262 TCXO (Heltec V3 — 1.6V, DC-DC) –––
#define LORA_TCXO_VOLTAGE       1.6f     // V – napięcie TCXO
#define LORA_USE_LDO            false    // false = DC-DC (zalecane)

// ––– Wartości dynamiczne – odczytywane z NVS; tu tylko fallback –––
#define LORA_FALLBACK_FREQUENCY 869.525f
#define LORA_FALLBACK_SF        11
#define LORA_FALLBACK_BW        250.0f
#define LORA_FALLBACK_CR        8        // 4/8

// ––– Monitoring kanału –––
#define RSSI_ACTIVITY_THRESHOLD  -105.0f  // dBm – powyżej = aktywność na kanale
#define RSSI_SAMPLE_INTERVAL_MS  200      // co ile próbkować RSSI
#define ACTIVITY_HOLD_MS         600      // jak długo pokazywać stan aktywny po zaniku

// ––– Piny – Heltec WiFi LoRa 32 V3 (ESP32-S3) –––
#define PIN_LORA_NSS   8
#define PIN_LORA_SCK   9
#define PIN_LORA_MOSI  10
#define PIN_LORA_MISO  11
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13
#define PIN_LORA_DIO1  14

#define PIN_OLED_SDA   17
#define PIN_OLED_SCL   18
#define PIN_OLED_RST   21
#define PIN_VEXT       36       // LOW = włącz zasilanie OLED + peryferia
#define PIN_LED        35
#define PIN_BUTTON     0

// ––– Wyświetlacz OLED –––
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_ADDR   0x3C

// ––– Firmware info –––
#define FIRMWARE_VERSION "1.0.0"
