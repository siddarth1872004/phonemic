#pragma once

#include "app.hpp"

struct ImFont;

namespace phonemic {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* big = nullptr;  // the phone's name
};

// Per-window UI state that isn't a user setting.
struct UiState {
    Fonts fonts;
    float scale = 1.0f;
    float level = 0, peak_hold = 0;
    uint64_t peak_hold_ms = 0;
    bool show_log = false;
    bool show_advanced = false;
    std::string pin;  // the Wi-Fi PIN being edited
    bool pin_loaded = false;
    int theme = -1, accent = -1;  // what setup_style() last applied
};

// Colours come from the theme (0 OLED black, 1 dark, 2 light) and accent.
void setup_style(float scale, int theme, int accent);
Fonts load_fonts(float scale);
void draw_ui(App& app, UiState& ui);

}  // namespace phonemic
