#pragma once
#include <Arduino.h>
#include <WiFi.h>

// ––– Tryb WiFi –––
enum WifiMode : uint8_t {
    WM_STA = 0,   // klient — łączy się do istniejącej sieci
    WM_AP         // access point — tworzy własną sieć
};

// ––– Konfiguracja WiFi –––
struct WifiConfig {
    WifiMode mode;
    char     ssid[33];       // max 32 znaki + null
    char     pass[65];       // max 64 znaki + null
};

// ––– API –––
void        wifi_init();                          // ładuje konfig z NVS i łączy/tworzy AP
void        wifi_check();                         // reconnect co 30s (tylko w STA)
String      wifi_status_str();

// ––– Runtime reconfiguration –––
bool        wifi_save_config(WifiMode mode, const char* ssid, const char* pass);
void        wifi_get_config(WifiConfig& cfg);      // odczytaj aktualną konfigurację
void        wifi_get_status_json(String& json);    // {"mode":"sta","ssid":"...","ip":"...","rssi":-67}
