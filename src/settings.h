#pragma once
#include <Arduino.h>

// ––– Profil radiowy –––
enum RadioProfile : uint8_t {
    PROFILE_MESHTASTIC = 0,   // sync 0x2B, wybór presetu
    PROFILE_MESHCORE,         // sync 0x12, SF8/BW62.5/CR4/8 (stałe)

    PROFILE_COUNT
};

// ––– Preset modemu Meshtastic (używane gdy profile == MESHTASTIC) –––
enum ModemPreset : uint8_t {
    PRESET_SHORT_FAST = 0,    // SF7,  BW250, CR4/8 — szybko, blisko
    PRESET_SHORT_SLOW,        // SF8,  BW250, CR4/8
    PRESET_MEDIUM_FAST,       // SF9,  BW250, CR4/8
    PRESET_MEDIUM_SLOW,       // SF10, BW250, CR4/8
    PRESET_LONG_FAST,         // SF11, BW250, CR4/8
    PRESET_LONG_SLOW,         // SF12, BW125, CR4/8
    PRESET_VERY_LONG_SLOW,    // SF12, BW62.5, CR4/8

    PRESET_COUNT
};

struct PresetInfo {
    uint8_t sf;
    float   bw_khz;
    uint8_t cr;         // 5=4/5, 6=4/6, 7=4/7, 8=4/8
    const char* name;   // ludzka nazwa
};

struct RadioSettings {
    RadioProfile profile;
    float        frequency;      // MHz
    ModemPreset  preset;         // tylko dla MESHTASTIC
    uint8_t      txPower;        // dBm (2..22, SX1262 max)
    float        tcxoVoltage;    // V – napięcie TCXO (1.6 lub 1.8)
    uint32_t     nodeId;         // ID węzła w sieci MeshCore/Meshtastic
};

// ––– Stałe MeshCore –––
#define MESHCORE_DEFAULT_FREQ   869.618f
#define MESHCORE_SF             8
#define MESHCORE_BW             62.5f
#define MESHCORE_CR             8        // 4/8

// ––– Domyślne ustawienia –––
#define DEFAULT_PROFILE         PROFILE_MESHCORE
#define DEFAULT_FREQUENCY       869.618f  // MHz — EU MeshCore
#define DEFAULT_PRESET          PRESET_LONG_FAST
#define DEFAULT_TX_POWER        15        // dBm (2..22 dla SX1262)
#define DEFAULT_TCXO_VOLTAGE    1.6f      // V — Heltec V3 używa 1.6V TCXO (większość rewizji)
#define DEFAULT_NODE_ID         0x00000001 // ID węzła (4 bajty, jak w Meshtastic)

// ––– API –––
void     settings_load();
void     settings_save();
void     settings_reset();

const PresetInfo& preset_get_info(ModemPreset p);
const char*       preset_get_label(ModemPreset p);

extern RadioSettings g_settings;

// ––– Opcje częstotliwości –––
#define FREQ_OPTION_COUNT 4
extern const float freqOptions[FREQ_OPTION_COUNT];
extern const char* freqLabels[FREQ_OPTION_COUNT];
