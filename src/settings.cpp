#include "settings.h"
#include <Preferences.h>

// ––– Tabela presetów –––
static const PresetInfo presets[PRESET_COUNT] = {
    //  sf   bw_kHz   cr    name
    {    7,  250.0f,   8,   "SHORT / FAST"      },
    {    8,  250.0f,   8,   "SHORT / SLOW"      },
    {    9,  250.0f,   8,   "MEDIUM / FAST"     },
    {   10,  250.0f,   8,   "MEDIUM / SLOW"     },
    {   11,  250.0f,   8,   "LONG / FAST"       },
    {   12,  125.0f,   8,   "LONG / SLOW"       },
    {   12,  62.5f,    8,   "VERY LONG / SLOW"  },
};

// ––– Opcje częstotliwości (MHz) –––
const float freqOptions[FREQ_OPTION_COUNT] = {
    868.000f,
    869.525f,
    869.618f,
    915.000f
};

const char* freqLabels[FREQ_OPTION_COUNT] = {
    "868.000 MHz (EU)",
    "869.525 MHz (EU)",
    "869.618 MHz (EU MeshCore)",
    "915.000 MHz (US)"
};

// ––– Globalny obiekt ustawień –––
RadioSettings g_settings = {
    DEFAULT_PROFILE,
    DEFAULT_FREQUENCY,
    DEFAULT_PRESET,
    DEFAULT_TX_POWER,
    DEFAULT_TCXO_VOLTAGE,
    DEFAULT_NODE_ID
};

// ––– NVS –––
static const char* NVS_NS = "lora";

void settings_load() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) {
        Serial.println(F("[Settings] Brak NVS — używam domyślnych."));
        settings_save();  // zapisz domyślne na przyszłość
        return;
    }

    // Wykryj migrację: jeśli NVS nie ma klucza "profile" — ustawienia ze starej wersji
    bool hasProfile = prefs.isKey("profile");

    g_settings.profile   = (RadioProfile)prefs.getUChar("profile", DEFAULT_PROFILE);
    g_settings.frequency = prefs.getFloat("freq", DEFAULT_FREQUENCY);
    g_settings.preset    = (ModemPreset)prefs.getUChar("preset", DEFAULT_PRESET);
    g_settings.txPower   = prefs.getUChar("txpower", DEFAULT_TX_POWER);
    g_settings.tcxoVoltage = prefs.getFloat("tcxoV", DEFAULT_TCXO_VOLTAGE);
    g_settings.nodeId    = prefs.getUInt("nodeId", DEFAULT_NODE_ID);

    // Jeśli stare ustawienia (brak profilu) — reset do domyślnych
    if (!hasProfile) {
        Serial.println(F("[Settings] Wykryto stare ustawienia — reset do domyślnych."));
        g_settings.profile   = DEFAULT_PROFILE;
        g_settings.frequency = DEFAULT_FREQUENCY;
        g_settings.preset    = DEFAULT_PRESET;
        g_settings.txPower   = DEFAULT_TX_POWER;
        g_settings.tcxoVoltage = DEFAULT_TCXO_VOLTAGE;
        g_settings.nodeId    = DEFAULT_NODE_ID;
        prefs.end();
        settings_save();
        return;
    }

    // Walidacja
    if (g_settings.profile >= PROFILE_COUNT) {
        g_settings.profile = DEFAULT_PROFILE;
    }
    if (g_settings.frequency < 100.0f || g_settings.frequency > 1000.0f) {
        g_settings.frequency = DEFAULT_FREQUENCY;
    }
    if (g_settings.preset >= PRESET_COUNT) {
        g_settings.preset = DEFAULT_PRESET;
    }
    if (g_settings.txPower < 2 || g_settings.txPower > 22) {
        g_settings.txPower = DEFAULT_TX_POWER;
    }
    if (g_settings.tcxoVoltage < 1.5f || g_settings.tcxoVoltage > 1.9f) {
        g_settings.tcxoVoltage = DEFAULT_TCXO_VOLTAGE;
    }

    prefs.end();

    const char* profileName = (g_settings.profile == PROFILE_MESHCORE) ? "MeshCore" : "Meshtastic";
    Serial.printf("[Settings] Wczytano: %s, %.3f MHz, %s, TX=%d dBm, TCXO=%.1f V, nodeId=0x%08X\n",
                  profileName, g_settings.frequency, preset_get_label(g_settings.preset), g_settings.txPower, g_settings.tcxoVoltage, g_settings.nodeId);
}

void settings_save() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar("profile", (uint8_t)g_settings.profile);
    prefs.putFloat("freq", g_settings.frequency);
    prefs.putUChar("preset", (uint8_t)g_settings.preset);
    prefs.putUChar("txpower", g_settings.txPower);
    prefs.putFloat("tcxoV", g_settings.tcxoVoltage);
    prefs.putUInt("nodeId", g_settings.nodeId);
    prefs.end();

    const char* profileName = (g_settings.profile == PROFILE_MESHCORE) ? "MeshCore" : "Meshtastic";
    Serial.printf("[Settings] Zapisano: %s, %.3f MHz, %s, TX=%d dBm, TCXO=%.1f V, nodeId=0x%08X\n",
                  profileName, g_settings.frequency, preset_get_label(g_settings.preset), g_settings.txPower, g_settings.tcxoVoltage, g_settings.nodeId);
}

void settings_reset() {
    g_settings.profile   = DEFAULT_PROFILE;
    g_settings.frequency = DEFAULT_FREQUENCY;
    g_settings.preset    = DEFAULT_PRESET;
    g_settings.txPower   = DEFAULT_TX_POWER;
    g_settings.tcxoVoltage = DEFAULT_TCXO_VOLTAGE;
    g_settings.nodeId    = DEFAULT_NODE_ID;
    settings_save();
}

const PresetInfo& preset_get_info(ModemPreset p) {
    if (p >= PRESET_COUNT) p = DEFAULT_PRESET;
    return presets[p];
}

const char* preset_get_label(ModemPreset p) {
    if (p >= PRESET_COUNT) p = DEFAULT_PRESET;
    return presets[p].name;
}
