#pragma once
#include <Arduino.h>

// ––– API –––
void display_init();
void display_show_splash();
void display_render();
void display_show_error(const char* msg);
void display_show_wifi_reset();

// ––– Tryby wyświetlania –––
enum DisplayMode : uint8_t {
    DISP_SPLASH = 0,
    DISP_IDLE,       // brak pakietów — czekamy
    DISP_PACKET,     // świeża ramka
    DISP_ERROR       // błąd krytyczny
};
