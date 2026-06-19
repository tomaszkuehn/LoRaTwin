#include "wifi_manager.h"
#include "config.h"
#include "display_handler.h"
#include <Preferences.h>

// ––– Domyślna konfiguracja –––
#define AP_SSID            "LoRaTwin"
#define AP_PASS            "12345678"
#define AP_IP              IPAddress(192, 168, 4, 1)
#define AP_GATEWAY         IPAddress(192, 168, 4, 1)
#define AP_SUBNET          IPAddress(255, 255, 255, 0)
#define WIFI_TIMEOUT_MS    20000
#define WIFI_RECONNECT_MS  30000

static const char* NVS_NS = "wifi";

// ––– Stan –––
static WifiConfig g_wifiCfg = {
    WM_AP,
    WIFI_SSID,              // z config.h
    WIFI_PASS               // z config.h
};
static unsigned long wifiReconnectMs = 0;
static bool wifiActive = false;

// ––– NVS: wczytaj konfigurację –––
static void wifi_load_config() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) {
        // Brak NVS — użyj domyślnych z config.h
        return;
    }

    if (prefs.isKey("mode")) {
        g_wifiCfg.mode = (WifiMode)prefs.getUChar("mode", WM_STA);
        String s = prefs.getString("ssid", WIFI_SSID);
        strncpy(g_wifiCfg.ssid, s.c_str(), 32);
        g_wifiCfg.ssid[32] = '\0';
        s = prefs.getString("pass", WIFI_PASS);
        strncpy(g_wifiCfg.pass, s.c_str(), 64);
        g_wifiCfg.pass[64] = '\0';
    }

    prefs.end();

    Serial.printf("[WiFi] Wczytano konfig: tryb=%s, ssid=%s\n",
                  g_wifiCfg.mode == WM_STA ? "STA" : "AP",
                  g_wifiCfg.ssid);
}

// ––– Inicjalizacja –––
void wifi_init() {
    wifi_load_config();

    if (g_wifiCfg.mode == WM_AP) {
        // ––– Tryb Access Point –––
        Serial.printf("[WiFi] Tryb AP: %s\n", AP_SSID);

        // softAPConfig() musi być PRZED softAP()
        if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
            Serial.println(F("[WiFi] OSTRZEŻENIE: nie udało się ustawić IP AP (używam domyślnego)."));
        }

        // WPA2 wymaga minimum 8 znaków hasła
        if (!WiFi.softAP(AP_SSID, AP_PASS)) {
            Serial.println(F("[WiFi] BŁĄD: nie udało się utworzyć AP!"));
            wifiActive = false;
            return;
        }

        delay(200);
        WiFi.setSleep(false);

        wifiActive = true;
        Serial.printf("[WiFi] AP gotowy: %s | IP: %s\n",
                      AP_SSID, WiFi.softAPIP().toString().c_str());
        Serial.printf("[WiFi] Stacje: max %d\n", WiFi.softAPgetStationNum());

    } else {
        // ––– Tryb STA (klient) –––
        Serial.printf("[WiFi] Tryb STA: łączę do %s...\n", g_wifiCfg.ssid);

        WiFi.mode(WIFI_MODE_STA);
        WiFi.setHostname("heltec-lora-rx");

        // Jeśli hasło jest puste — sieć otwarta
        if (strlen(g_wifiCfg.pass) > 0) {
            WiFi.begin(g_wifiCfg.ssid, g_wifiCfg.pass);
        } else {
            WiFi.begin(g_wifiCfg.ssid);
        }

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
            Serial.print('.');
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println();
            Serial.printf("[WiFi] Połączono! IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
            wifiActive = true;
        } else {
            Serial.println();
            Serial.println(F("[WiFi] NIE połączono — działam bez sieci."));
            wifiActive = false;
        }
    }

    wifiReconnectMs = millis();
}

// ––– Reconnect / keepalive –––
void wifi_check() {
    if (g_wifiCfg.mode != WM_STA) return;  // AP nie potrzebuje reconnect

    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifiReconnectMs > WIFI_RECONNECT_MS) {
            wifiReconnectMs = millis();
            Serial.println(F("[WiFi] Próba ponownego połączenia..."));
            WiFi.reconnect();
            wifiActive = (WiFi.status() == WL_CONNECTED);
        }
    } else {
        wifiActive = true;
    }
}

String wifi_status_str() {
    if (g_wifiCfg.mode == WM_AP) {
        return "AP: " + WiFi.softAPIP().toString();
    }
    if (WiFi.status() == WL_CONNECTED) {
        return "Connected: " + WiFi.localIP().toString();
    }
    return "Not connected";
}

// ––– Runtime reconfiguration –––
bool wifi_save_config(WifiMode mode, const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
    if (pass && strlen(pass) > 64) return false;

    // Zapisz do NVS
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) {
        Serial.println(F("[WiFi] BŁĄD: nie można otworzyć NVS do zapisu!"));
        return false;
    }

    prefs.putUChar("mode", (uint8_t)mode);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass ? pass : "");

    prefs.end();

    // Aktualizuj RAM
    g_wifiCfg.mode = mode;
    strncpy(g_wifiCfg.ssid, ssid, 32);
    g_wifiCfg.ssid[32] = '\0';
    if (pass) {
        strncpy(g_wifiCfg.pass, pass, 64);
        g_wifiCfg.pass[64] = '\0';
    } else {
        g_wifiCfg.pass[0] = '\0';
    }

    Serial.printf("[WiFi] Zapisano nową konfig: tryb=%s, ssid=%s\n",
                  mode == WM_STA ? "STA" : "AP",
                  ssid);

    // Płynne przełączenie trybu WiFi — bez wyłączania radia,
    // bez utraty stosu TCP/IP. Serwer WWW pozostaje dostępny.
    if (mode == WM_AP) {
        // STA → AP: rozłącz STA (nie wyłączaj radia), uruchom AP
        WiFi.disconnect(false);
        delay(100);
        WiFi.mode(WIFI_MODE_AP);
        if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
            Serial.println(F("[WiFi] OSTRZEŻENIE: nie udało się ustawić IP AP"));
        }
        if (!WiFi.softAP(AP_SSID, AP_PASS)) {
            Serial.println(F("[WiFi] BŁĄD: nie udało się utworzyć AP!"));
            wifiActive = false;
            return false;
        }
        delay(200);
        WiFi.setSleep(false);
        wifiActive = true;
        Serial.printf("[WiFi] Przełączono na AP: %s | IP: %s\n",
                      AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        // AP → STA: wyłącz AP, uruchom klienta (bez blokowania — łączenie w tle)
        WiFi.softAPdisconnect(false);
        delay(100);
        WiFi.mode(WIFI_MODE_STA);
        WiFi.setHostname("heltec-lora-rx");
        if (strlen(g_wifiCfg.pass) > 0) {
            WiFi.begin(g_wifiCfg.ssid, g_wifiCfg.pass);
        } else {
            WiFi.begin(g_wifiCfg.ssid);
        }
        wifiActive = true;
        wifiReconnectMs = millis();
        Serial.printf("[WiFi] Przełączono na STA: łączę do %s (w tle)...\n", g_wifiCfg.ssid);
    }

    return true;
}

void wifi_get_config(WifiConfig& cfg) {
    cfg = g_wifiCfg;
}

void wifi_reset_to_defaults() {
    Serial.println(F("[WiFi] Reset do ustawień domyślnych (AP:LoRaTwin)..."));

    // Wyczyść NVS — usuń całą przestrzeń nazw "wifi"
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();

    // Przywróć domyślną konfigurację AP w RAM
    g_wifiCfg.mode = WM_AP;
    strncpy(g_wifiCfg.ssid, AP_SSID, 32);
    g_wifiCfg.ssid[32] = '\0';
    strncpy(g_wifiCfg.pass, AP_PASS, 64);
    g_wifiCfg.pass[64] = '\0';

    // Rozłącz obecne WiFi i zresetuj kontroler
    WiFi.disconnect(true);
    delay(500);

    // Restart w trybie AP
    WiFi.mode(WIFI_MODE_AP);

    if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
        Serial.println(F("[WiFi] OSTRZEŻENIE: nie udało się ustawić IP AP (używam domyślnego)."));
    }

    if (!WiFi.softAP(AP_SSID, AP_PASS)) {
        Serial.println(F("[WiFi] BŁĄD: nie udało się utworzyć AP!"));
        wifiActive = false;
        return;
    }

    delay(200);
    WiFi.setSleep(false);
    wifiActive = true;

    Serial.printf("[WiFi] AP gotowy (po resecie): %s | IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
}

void wifi_get_status_json(String& json) {
    json.reserve(512);  // SSID(32) + IP(15) + JSON framing ~150B, 512 safe
    json = '{';
    json += "\"mode\":\"";
    json += (g_wifiCfg.mode == WM_AP) ? "ap" : "sta";
    json += "\",";
    json += "\"ssid\":\"";
    json += g_wifiCfg.ssid;
    json += "\",";

    if (g_wifiCfg.mode == WM_AP) {
        json += "\"ip\":\"";
        json += WiFi.softAPIP().toString();
        json += "\",";
        json += "\"rssi\":0,";
        json += "\"stations\":";
        json += WiFi.softAPgetStationNum();
    } else {
        json += "\"ip\":\"";
        json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
        json += "\",";
        json += "\"rssi\":";
        json += (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        json += ",";
        json += "\"connected\":";
        json += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
    }
    json += '}';
}
