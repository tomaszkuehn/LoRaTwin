/**
 * Heltec WiFi LoRa 32 V3 — Odbiornik LoRa + Web Loader
 *
 * Funkcje:
 *  - Odbiór ramek LoRa (SX1262) i wyświetlanie na OLED (SSD1306 128×64)
 *  - Serwer WWW z dashboardem stanu + loaderem firmware (ElegantOTA)
 *  - API REST /api/status z pełnym stanem urządzenia (JSON)
 *
 * Sprzęt: ESP32-S3, Heltec WiFi LoRa 32 V3
 * Częstotliwość domyślna: 868 MHz (EU) – zmień w config.h dla US (915 MHz)
 */

#include "config.h"
#include "wifi_manager.h"
#include "lora_handler.h"
#include "display_handler.h"
#include "web_server.h"
#include <ElegantOTA.h>
#include <LittleFS.h>

// ====================================================================
static void setup_serial();
static void setup_power();
static void setup_led();
// ====================================================================

void setup() {
    // 1. Port szeregowy
    setup_serial();

    // 2. Zasilanie peryferiów (Vext = LOW włącza OLED)
    setup_power();

    // 3. Konfiguracja LED
    setup_led();

    // 4. Wyświetlacz OLED
    display_init();

    // 5. Radio LoRa
    if (!lora_init()) {
        display_show_error("LoRa init failed!\nCheck wiring & freq.");
        Serial.println(F("FATAL: LoRa init failed. Halted."));
        while (1) {
            delay(1000);
        }
    }

    // 6. WiFi (STA lub AP — konfiguracja z NVS / web UI)
    wifi_init();

    // 7. Serwer HTTP + ElegantOTA
    web_server_init();

    // 8. Raport końcowy
    Serial.println(F("\n============================================"));
    Serial.println(F("  LoRaTwin v" FIRMWARE_VERSION));
    Serial.println(F("============================================"));
    Serial.printf("  LoRa: %.3f MHz, SF%d, BW %.0f kHz\n",
                  lora_get_frequency(), lora_get_sf(), lora_get_bandwidth());
    WifiConfig wcfg;
    wifi_get_config(wcfg);
    if (wcfg.mode == WM_AP) {
        Serial.printf("  WiFi: AP mode | SSID: %s | IP: %s\n",
                      wcfg.ssid, WiFi.softAPIP().toString().c_str());
        Serial.printf("  OTA:  http://%s/update\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("  Dashboard: http://%s/\n", WiFi.softAPIP().toString().c_str());
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  WiFi: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  OTA:  http://%s/update\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(F("  WiFi: NOT CONNECTED (web UI niedostępne)"));
    }
    Serial.println(F("============================================\n"));
}

void loop() {
    // ElegantOTA — obsługa uploadu firmware (musi być w loop)
    ElegantOTA.loop();

    // Sprawdzenie stanu WiFi + ewentualny reconnect
    wifi_check();

    // Odbiór ramek LoRa (ISR → flaga → odczyt w loop)
    lora_process();

    // Non-blocking LED blink handling
    lora_led_blink_update();
    if (!lora_led_blink_active()) {
        // LED — świeci tylko przy poprawnej ramce (ostatnie 2s)
        digitalWrite(PIN_LED, lora_has_rx() ? HIGH : LOW);
    }

    // Statystyki per-minute
    stats_tick();

    // Odświeżenie wyświetlacza OLED (ratelimit ~10 Hz)
    display_render();
}

// ====================================================================
static void setup_serial() {
    Serial.begin(115200);
    delay(1200);   // Daj czas USB-to-Serial na stabilizację
    Serial.println();
    Serial.println(F("=== Heltec WiFi LoRa 32 V3 — Boot ==="));
}

static void setup_power() {
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);    // LOW = włącz zasilanie OLED i peryferiów
    delay(100);                     // Daj czas na stabilizację napięcia
}

static void setup_led() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);     // LED domyślnie wyłączony
}
