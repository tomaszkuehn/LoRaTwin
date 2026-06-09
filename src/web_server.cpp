#include "web_server.h"
#include "config.h"
#include "lora_handler.h"
#include "settings.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>

static AsyncWebServer server(80);

// ––– GET /api/wifi — pobierz konfigurację WiFi –––
static void handleApiWifiGet(AsyncWebServerRequest* request) {
    String json;
    wifi_get_status_json(json);
    request->send(200, "application/json", json);
}

// ––– POST /api/wifi — zapisz konfigurację WiFi i przełącz tryb –––
static void handleApiWifiPost(AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) {
        request->send(400, "text/plain", "Missing ssid");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true)
                    ? request->getParam("pass", true)->value()
                    : "";
    int mode = request->hasParam("mode", true)
                ? request->getParam("mode", true)->value().toInt()
                : 0;  // domyślnie STA

    if (ssid.length() == 0 || ssid.length() > 32) {
        request->send(400, "text/plain", "SSID too long or empty (max 32)");
        return;
    }
    if (pass.length() > 64) {
        request->send(400, "text/plain", "Password too long (max 64)");
        return;
    }
    if (mode < 0 || mode > 1) {
        request->send(400, "text/plain", "Invalid mode (0=STA, 1=AP)");
        return;
    }

    bool ok = wifi_save_config((WifiMode)mode, ssid.c_str(),
                                pass.length() > 0 ? pass.c_str() : nullptr);

    DynamicJsonDocument doc(512);
    doc["success"] = ok;
    doc["mode"] = mode;
    doc["ssid"] = ssid;

    String json;
    serializeJson(doc, json);
    request->send(ok ? 200 : 500, "application/json", json);
}

// ––– GET /api/status — JSON ze stanem urządzenia –––
static void handleApiStatus(AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(1024);

    doc["chip"]         = "ESP32-S3";
    doc["flash_mb"]     = ESP.getFlashChipSize() / (1024 * 1024);
    doc["firmware"]     = FIRMWARE_VERSION;
    doc["uptime_s"]     = millis() / 1000;
    doc["free_heap"]    = ESP.getFreeHeap();
    doc["wifi_ssid"]    = WIFI_SSID;
    doc["wifi_rssi"]    = WiFi.RSSI();
    doc["ip"]           = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0";
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["lora_freq_mhz"]= lora_get_frequency();
    doc["lora_sf"]      = lora_get_sf();
    doc["lora_bw_khz"]  = lora_get_bandwidth();
    doc["lora_cr"]      = lora_get_cr();
    doc["packets_received"] = packetCount;
    doc["current_rssi"] = currentRssi;
    doc["last_rssi"]    = lastRssi;
    doc["last_snr"]     = lastSnr;
    doc["freq_error_hz"]= lastFreqError;

    String json;
    serializeJson(doc, json);

    request->send(200, "application/json", json);
}

// ––– GET /api/config — pobierz konfigurację radia –––
static void handleApiConfigGet(AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(768);
    doc["profile"]    = (uint8_t)g_settings.profile;
    doc["frequency"]  = g_settings.frequency;
    doc["preset"]     = (uint8_t)g_settings.preset;
    doc["preset_name"] = preset_get_label(g_settings.preset);
    doc["tx_power"]   = g_settings.txPower;
    doc["tcxo"]       = g_settings.tcxoVoltage;
    doc["node_id"]    = g_settings.nodeId;
    doc["sf"]         = lora_get_sf();
    doc["bw_khz"]     = lora_get_bandwidth();
    doc["cr"]         = lora_get_cr();
    doc["sync_word"]  = (g_settings.profile == PROFILE_MESHCORE) ? LORA_SYNC_MESHCORE : LORA_SYNC_MESHTASTIC;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ––– POST /api/config — zapisz konfigurację radia –––
static void handleApiConfigPost(AsyncWebServerRequest* request) {
    if (!request->hasParam("frequency", true) || !request->hasParam("preset", true)) {
        request->send(400, "text/plain", "Missing frequency or preset");
        return;
    }

    float freq = request->getParam("frequency", true)->value().toFloat();
    int preset = request->getParam("preset", true)->value().toInt();

    // Walidacja
    if (freq < 100.0f || freq > 1000.0f || preset < 0 || preset >= PRESET_COUNT) {
        request->send(400, "text/plain", "Invalid frequency or preset");
        return;
    }

    // Profil (opcjonalny)
    if (request->hasParam("profile", true)) {
        int prof = request->getParam("profile", true)->value().toInt();
        if (prof >= 0 && prof < PROFILE_COUNT) {
            g_settings.profile = (RadioProfile)prof;
        }
    }

    g_settings.frequency = freq;
    g_settings.preset    = (ModemPreset)preset;

    // Moc TX (opcjonalna, 2-22 dBm)
    if (request->hasParam("tx_power", true)) {
        int txp = request->getParam("tx_power", true)->value().toInt();
        if (txp >= 2 && txp <= 22) {
            g_settings.txPower = (uint8_t)txp;
        }
    }

    // TCXO (opcjonalne, 1.5-1.9 V)
    if (request->hasParam("tcxo", true)) {
        float tcxo = request->getParam("tcxo", true)->value().toFloat();
        if (tcxo >= 1.5f && tcxo <= 1.9f) {
            g_settings.tcxoVoltage = tcxo;
        }
    }

    // Node ID (opcjonalne, hex string jak "00000001" lub decimal)
    if (request->hasParam("node_id", true)) {
        String nodeIdStr = request->getParam("node_id", true)->value();
        uint32_t nid = (uint32_t)strtoul(nodeIdStr.c_str(), NULL, 0);
        if (nid != 0) {
            g_settings.nodeId = nid;
        }
    }

    settings_save();

    // Restart radia z nowymi parametrami
    bool ok = lora_reinit();

    DynamicJsonDocument doc(768);
    doc["success"] = ok;
    doc["profile"] = (uint8_t)g_settings.profile;
    doc["frequency"] = g_settings.frequency;
    doc["preset"] = (uint8_t)g_settings.preset;
    doc["preset_name"] = preset_get_label(g_settings.preset);
    doc["tx_power"] = g_settings.txPower;
    doc["tcxo"] = g_settings.tcxoVoltage;
    doc["node_id"] = g_settings.nodeId;

    String json;
    serializeJson(doc, json);
    request->send(ok ? 200 : 500, "application/json", json);
}

// ––– JSON helper: append escaped string –––
static void appendJsonStr(String& json, const char* s) {
    json += '"';
    while (*s) {
        char c = *s++;
        if (c == '"')      json += F("\\\"");
        else if (c == '\\') json += F("\\\\");
        else if (c >= 0x20) json += c;   // skip control chars
    }
    json += '"';
}

// ––– GET /api/log — pobierz log zdarzeń (max 30 ostatnich wpisów) –––
static void handleApiLog(AsyncWebServerRequest* request) {
    const int LIMIT = 30;
    String json;
    json.reserve(16384);

    json += F("{\"events\":[");
    if (logCount > 0) {
        int total = logCount;
        int count = (total < LIMIT) ? total : LIMIT;
        int skip  = total - count;
        int start = (total < LOG_CAPACITY) ? skip
                   : (logHead + 1 + skip) % LOG_CAPACITY;
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % LOG_CAPACITY;
            if (i > 0) json += ',';

            json += F("{\"t\":");
            json += logBuffer[idx].timestamp;
            json += F(",\"type\":\"");
            json += logBuffer[idx].type;
            json += F("\",\"len\":");
            json += logBuffer[idx].len;
            json += F(",\"rssi\":");
            json += logBuffer[idx].rssi;
            json += F(",\"snr\":");
            json += logBuffer[idx].snr;
            json += F(",\"proto\":\"");
            json += proto_to_str(logBuffer[idx].proto);
            json += '"';

            if (logBuffer[idx].proto == PROTO_MESHCORE) {
                json += F(",\"mcRoute\":\"");
                json += mc_route_type_name(logBuffer[idx].mcRouteType);
                json += F("\",\"mcRouteId\":");
                json += logBuffer[idx].mcRouteType;
                json += F(",\"mcPayload\":\"");
                json += mc_payload_type_name(logBuffer[idx].mcPayloadType);
                json += F("\",\"mcPayloadId\":");
                json += logBuffer[idx].mcPayloadType;
                json += F(",\"mcVer\":");
                json += logBuffer[idx].mcPayloadVer + 1;
                json += F(",\"mcHops\":");
                json += logBuffer[idx].mcHopCount;
                json += F(",\"mcHashSz\":");
                json += logBuffer[idx].mcHashSize;
                json += F(",\"mcPathLen\":");
                json += logBuffer[idx].mcHopCount * logBuffer[idx].mcHashSize;
                json += F(",\"mcTransport\":");
                json += logBuffer[idx].mcHasTransport ? "true" : "false";
                if (logBuffer[idx].mcHasTransport) {
                    json += F(",\"mcTC1\":");
                    json += logBuffer[idx].mcTransport1;
                    json += F(",\"mcTC2\":");
                    json += logBuffer[idx].mcTransport2;
                }
                if (logBuffer[idx].dataLen > 0 && logBuffer[idx].mcPayloadType <= 0x0F) {
                    MeshCoreInfo tmpInfo;
                    tmpInfo.payloadType = logBuffer[idx].mcPayloadType;
                    tmpInfo.routeType = logBuffer[idx].mcRouteType;
                    tmpInfo.payloadVersion = logBuffer[idx].mcPayloadVer;
                    tmpInfo.hopCount = logBuffer[idx].mcHopCount;
                    tmpInfo.hashSize = logBuffer[idx].mcHashSize;
                    tmpInfo.hasTransport = logBuffer[idx].mcHasTransport;
                    tmpInfo.pathLen = logBuffer[idx].mcHopCount * logBuffer[idx].mcHashSize;
                    uint8_t off = 1;
                    if (tmpInfo.hasTransport) off += 4;
                    off += 1;
                    off += tmpInfo.pathLen;
                    tmpInfo.payloadOffset = off;
                    char payloadBuf[128];
                    decode_payload_summary(logBuffer[idx].data, logBuffer[idx].dataLen,
                                           tmpInfo, payloadBuf, sizeof(payloadBuf));
                    json += F(",\"mcPayloadTxt\":");
                    appendJsonStr(json, payloadBuf);
                }
            }
            if (logBuffer[idx].dataLen > 0) {
                static char hex[LOG_DATA_MAX * 3 + 1];
                int pos = 0;
                for (uint8_t b = 0; b < logBuffer[idx].dataLen; b++) {
                    pos += sprintf(hex + pos, "%02X ", logBuffer[idx].data[b]);
                }
                hex[pos > 0 ? pos - 1 : 0] = '\0';  // remove trailing space
                json += F(",\"hex\":");
                appendJsonStr(json, hex);
            }
            json += '}';
        }
    }
    json += F("]}");
    request->send(200, "application/json", json);
}

// ––– GET /api/log/simple — uproszczony log (max 50 ostatnich wpisów) –––
static void handleApiLogSimple(AsyncWebServerRequest* request) {
    const int LIMIT = 50;
    String json;
    json.reserve(14336);

    json += F("{\"events\":[");
    if (simpleLogCount > 0) {
        int total = simpleLogCount;
        int count = (total < LIMIT) ? total : LIMIT;
        int skip  = total - count;
        int start = (total < SIMPLE_LOG_CAPACITY) ? skip
                   : (simpleLogHead + 1 + skip) % SIMPLE_LOG_CAPACITY;
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % SIMPLE_LOG_CAPACITY;
            if (i > 0) json += ',';

            json += F("{\"t\":");
            json += simpleLogBuffer[idx].timestamp;
            json += F(",\"type\":\"");
            json += simpleLogBuffer[idx].type;
            json += F("\",\"rssi\":");
            json += simpleLogBuffer[idx].rssi;
            json += F(",\"snr\":");
            json += simpleLogBuffer[idx].snr;
            json += F(",\"route\":\"");
            json += (simpleLogBuffer[idx].routeType != 0xFF)
                  ? mc_route_type_name(simpleLogBuffer[idx].routeType) : "?";
            json += F("\",\"routeId\":");
            json += simpleLogBuffer[idx].routeType;
            json += F(",\"payload\":\"");
            json += (simpleLogBuffer[idx].payloadType != 0xFF)
                  ? mc_payload_type_name(simpleLogBuffer[idx].payloadType) : "?";
            json += F("\",\"payloadId\":");
            json += simpleLogBuffer[idx].payloadType;
            json += F(",\"hops\":");
            json += simpleLogBuffer[idx].hopCount;
            json += F(",\"text\":");
            if (simpleLogBuffer[idx].text[0]) {
                appendJsonStr(json, simpleLogBuffer[idx].text);
            } else {
                json += F("\"\"");
            }
            json += '}';
        }
    }
    json += F("]}");
    request->send(200, "application/json", json);
}

// ––– POST /api/tx — wyślij ramkę LoRa –––
static void handleApiTx(AsyncWebServerRequest* request) {
    if (!request->hasParam("data", true)) {
        request->send(400, "text/plain", "Missing data");
        return;
    }

    String payload = request->getParam("data", true)->value();
    if (payload.length() == 0 || payload.length() > 200) {
        request->send(400, "text/plain", "Payload too long (max 200)");
        return;
    }

    bool ok = lora_tx((const uint8_t*)payload.c_str(), payload.length());

    DynamicJsonDocument doc(256);
    doc["success"] = ok;
    doc["bytes"]   = payload.length();

    String json;
    serializeJson(doc, json);
    request->send(ok ? 200 : 500, "application/json", json);
}

// ––– GET / — strona główna (dashboard) –––
static void handleRoot(AsyncWebServerRequest* request) {
    // Serwuj index.html z LittleFS, jeśli istnieje
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
    } else {
        // Fallback: prosta strona HTML inline
        request->send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Heltec LoRa RX - No LittleFS</title>
    <style>
        body { font-family: sans-serif; background: #1a1a2e; color: #e0e0e0;
               max-width: 600px; margin: 2em auto; padding: 1em; }
        h1 { color: #00d4ff; }
        .warn { background: #332200; border: 1px solid #ffaa00; padding: 1em; border-radius: 8px; }
        a { color: #00d4ff; }
    </style>
</head>
<body>
    <h1>Heltec LoRa Receiver</h1>
    <div class="warn">
        <strong>LittleFS nie został wgrany.</strong><br>
        Wgraj obraz systemu plików: <code>pio run -t uploadfs</code>
    </div>
    <h2>Stan urządzenia</h2>
    <ul>
        <li>Firmware: vFIRMWARE_VERSION</li>
        <li>Chip: ESP32-S3</li>
        <li>WiFi: <span id="wifi">...</span></li>
        <li>Pakiety LoRa: <span id="pkts">...</span></li>
    </ul>
    <p><a href="/update">Firmware Upload (ElegantOTA)</a></p>
    <script>
        fetch('/api/status').then(r=>r.json()).then(d=>{
            document.getElementById('wifi').textContent = d.ip;
            document.getElementById('pkts').textContent = d.packets_received;
        });
    </script>
</body>
</html>
        )rawliteral");
    }
}

// ––– 404 –––
static void handleNotFound(AsyncWebServerRequest* request) {
    if (request->method() == HTTP_OPTIONS) {
        request->send(200);
        return;
    }
    request->send(404, "text/plain", "404 Not Found");
}

// ––– Inicjalizacja serwera –––
void web_server_init() {
    Serial.println(F("[Web] Inicjalizacja serwera HTTP..."));

    // LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println(F("[Web] OSTRZEŻENIE: Nie udało się zamontować LittleFS!"));
        Serial.println(F("[Web] Strona dashboard będzie niedostępna."));
        Serial.println(F("[Web] Wgraj system plików: pio run -t uploadfs"));
    } else {
        uint32_t total = LittleFS.totalBytes();
        uint32_t used  = LittleFS.usedBytes();
        Serial.printf("[Web] LittleFS zamontowany: %u / %u KB użyte\n", used / 1024, total / 1024);
    }

    // Endpointy
    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/api/status",  HTTP_GET,  handleApiStatus);
    server.on("/api/config",  HTTP_GET,  handleApiConfigGet);
    server.on("/api/config",  HTTP_POST, handleApiConfigPost);
    server.on("/api/wifi",    HTTP_GET,  handleApiWifiGet);
    server.on("/api/wifi",    HTTP_POST, handleApiWifiPost);
    server.on("/api/tx",      HTTP_POST, handleApiTx);
    server.on("/api/simplelog", HTTP_GET,  handleApiLogSimple);
    server.on("/api/log",     HTTP_GET,  handleApiLog);
    server.onNotFound(handleNotFound);

    // CORS — pozwól na zapytania z dowolnego źródła (przydatne podczas dev)
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // ElegantOTA — dostępny pod /update
    ElegantOTA.begin(&server);
    // Hasło domyślne (opcjonalnie zmień):
    // ElegantOTA.setAuth("admin", "admin123");

    server.begin();
    Serial.println(F("[Web] Serwer HTTP uruchomiony na porcie 80."));
    Serial.printf("[Web] Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("[Web] OTA:       http://%s/update\n", WiFi.localIP().toString().c_str());
}
