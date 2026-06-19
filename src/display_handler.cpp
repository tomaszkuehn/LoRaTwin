#include "display_handler.h"
#include "config.h"
#include "lora_handler.h"
#include "settings.h"
#include "wifi_manager.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>

// ––– Globalny tryb wyświetlacza –––
static DisplayMode dispMode = DISP_SPLASH;
static uint32_t     lastRenderMs = 0;
static uint32_t     splashEndMs  = 0;
static uint32_t     wifiResetEndMs = 0;   // timeout powrotu po komunikacie resetu WiFi


// ––– Konstruktor U8g2 –––
// HW I2C, używamy zewnętrznie skonfigurowanego Wire
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,          // brak rotacji
    PIN_OLED_RST);    // pin resetu

// ––– Inicjalizacja –––
void display_init() {
    Serial.println(F("[Display] Inicjalizacja OLED..."));

    // Konfiguracja I2C — jawnie ustawiamy piny SDA/SCL
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(400000);        // 400 kHz – szybki tryb I2C

    // Inicjalizacja U8g2
    if (!u8g2.begin()) {
        Serial.println(F("[Display] BŁĄD: nie wykryto SSD1306!"));
        return;
    }

    u8g2.setFont(u8g2_font_5x7_tf);   // mała, czytelna czcionka (5×7 px)
    u8g2.setFontRefHeightExtendedText();
    u8g2.setDrawColor(1);
    u8g2.setFontPosBottom();            // współrzędne kursora = baseline

    Serial.println(F("[Display] OLED gotowy."));

    // Pokaż ekran startowy na 2.5 s
    display_show_splash();
}

// ––– Ekran startowy –––
void display_show_splash() {
    dispMode    = DISP_SPLASH;
    splashEndMs = millis() + 4000;

    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(28, 15, "LoRaTwin");

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(10, 30);
    u8g2.printf("%.1f MHz  SF%d  CR4/%d",
                lora_get_frequency(), lora_get_sf(), lora_get_cr());

    u8g2.drawStr(20, 44, "software by");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(28, 58, "SP3FHI");

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(50, 64, "v" FIRMWARE_VERSION);

    u8g2.sendBuffer();
}

// ––– Główna funkcja renderująca –– wołana z loop() –––
void display_render() {
    // Ratelimit odświeżania: ~10 Hz
    uint32_t now = millis();
    if (now - lastRenderMs < 100) return;
    lastRenderMs = now;

    // Podczas splashu — nie rysuj nic, czekaj aż się skończy
    if (dispMode == DISP_SPLASH) {
        if (now > splashEndMs) {
            dispMode = DISP_IDLE;
        } else {
            return;   // <-- splash wciąż trwa, nie nadpisuj
        }
    }

    // Po komunikacie resetu WiFi — wróć do normalnego trybu po 3s
    if (dispMode == DISP_ERROR && wifiResetEndMs > 0 && now > wifiResetEndMs) {
        dispMode = DISP_IDLE;
        wifiResetEndMs = 0;
    }

    // Sprawdź, czy czeka nowy pakiet w kolejce
    LoraPacket pkt;
    bool hasPacket = packetQueue.pop(pkt);

    if (hasPacket) {
        dispMode = DISP_PACKET;
        // Jeśli pakietów jest dużo — opróżnij kolejkę do najnowszego
        while (packetQueue.pop(pkt)) { /* zostawiamy ostatni */ }
    }

    u8g2.clearBuffer();

    // ––– LINIA 1: LoRaTwin + tryb (y=9, font 6x10) –––
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 9);
    u8g2.print("LoRaTwin: ");
    u8g2.print((g_settings.profile == PROFILE_MESHCORE) ? "MeshCore" : "Meshtastic");

    // ––– LINIA 2: Status (y=21) –––
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(0, 21);
    u8g2.printf("%.1fM  SF%d", lora_get_frequency(), lora_get_sf());
    u8g2.setCursor(78, 21);
    u8g2.printf("#%u", packetCount);

    // Kontrolka historii (latch)
    u8g2.drawFrame(100, 14, 6, 7);
    if (lora_has_history()) {
        u8g2.drawBox(101, 15, 4, 5);
    }

    // Ikona aktywności LoRa
    RadioActivity act = lora_get_activity();
    switch (act) {
        case ACT_IDLE:
            u8g2.drawFrame(109, 14, 7, 7);
            break;
        case ACT_ENERGY:
            u8g2.drawFrame(109, 14, 7, 7);
            u8g2.drawPixel(112, 17);
            break;
        case ACT_RECEIVING:
            u8g2.drawBox(109, 14, 7, 7);
            break;
        case ACT_CRC_FAIL:
            u8g2.drawFrame(109, 14, 7, 7);
            u8g2.drawLine(110, 15, 114, 19);
            u8g2.drawLine(114, 15, 110, 19);
            break;
    }

    // ––– LINIA 3: RSSI / SNR (y=34) –––
    u8g2.setCursor(0, 34);
    u8g2.printf("RSSI:%.0f dBm  SNR:%.1f dB", currentRssi, lastSnr);

    // ––– LINIA 4: RX counter + protokół + payload type (y=43) –––
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 46);
    const char* protoLabel = "?";
    switch (lastProto) {
        case PROTO_MESHCORE:   protoLabel = "MC"; break;
        case PROTO_MESHTASTIC: protoLabel = "MT"; break;
        case PROTO_OTHER:      protoLabel = "??"; break;
        default:               break;
    }
    if (lastProto == PROTO_MESHCORE && lastMcInfo.payloadType <= 0x0F) {
        u8g2.printf("RX:%u %s/%s", packetCount, protoLabel,
                    mc_payload_type_name(lastMcInfo.payloadType));
    } else {
        u8g2.printf("RX: %u %s", packetCount, protoLabel);
    }
    u8g2.setFont(u8g2_font_5x7_tf);

    // ––– Stopka — IP / status (y=63) –––
    u8g2.setCursor(0, 63);
    WifiConfig wcfg;
    wifi_get_config(wcfg);
    if (wcfg.mode == WM_AP) {
        u8g2.print("AP: ");
        u8g2.print(WiFi.softAPIP().toString());
    } else if (WiFi.status() == WL_CONNECTED) {
        u8g2.print("IP: ");
        u8g2.print(WiFi.localIP().toString());
    } else {
        u8g2.print("WiFi: not connected");
    }

    u8g2.sendBuffer();
}

// ––– Ekran błędu krytycznego –––
void display_show_error(const char* msg) {
    dispMode = DISP_ERROR;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(10, 18, "ERROR");

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(0, 35);

    // Zawijanie długich komunikatów (UTF-8 safe)
    const char* p = msg;
    uint8_t line = 0;
    while (*p && line < 4) {
        size_t len = strlen(p);
        if (len > 25) {
            len = 25;
            // Don't split UTF-8: walk back from truncation past continuation bytes,
            // find the nearest start byte that extends past len, drop it
            size_t check = len;
            while (check > 0 && ((p[check] & 0xC0) == 0x80)) check--;
            if (check > 0) {
                unsigned char b = (unsigned char)p[check - 1];
                uint8_t seqLen = 0;
                if      ((b & 0xE0) == 0xC0) seqLen = 2;
                else if ((b & 0xF0) == 0xE0) seqLen = 3;
                else if ((b & 0xF8) == 0xF0) seqLen = 4;
                if (seqLen > 0 && check - 1 + seqLen > len) {
                    len = check - 1;  // drop incomplete multi-byte char
                }
            }
        }
        char buf[32];
        memcpy(buf, p, len);
        buf[len] = '\0';
        u8g2.setCursor(0, 35 + line * 9);
        u8g2.print(buf);
        p += len;
        line++;
    }

    u8g2.drawStr(0, 63, "Reset the device...");
    u8g2.sendBuffer();
}

// ––– Komunikat resetu WiFi (długie naciśnięcie PRG) –––
void display_show_wifi_reset() {
    dispMode = DISP_ERROR;   // użyj trybu błędu aby zablokować normalne renderowanie
    wifiResetEndMs = millis() + 3000;  // po 3s wróć do normalnego wyświetlania

    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(18, 18, "WiFi RESET");

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(10, 36);
    u8g2.print(F("AP: LoRaTwin"));
    u8g2.setCursor(10, 48);
    u8g2.print(F("IP: 192.168.4.1"));

    u8g2.setCursor(10, 63);
    u8g2.print(F("Rebooting WiFi..."));

    u8g2.sendBuffer();
}
