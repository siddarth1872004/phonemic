#pragma once

#include "app.hpp"

struct ImFont;

namespace phonemic {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* big = nullptr;  // the PC's Bluetooth name
};

// Per-window UI state that isn't a user setting.
struct UiState {
    Fonts fonts;
    float scale = 1.0f;
    float level = 0, peak_hold = 0;
    uint64_t peak_hold_ms = 0;
    bool show_log = false;
    bool show_advanced = false;
};

void setup_style(float scale);
Fonts load_fonts(float scale);
void draw_ui(App& app, UiState& ui);

}  // namespace phonemic
