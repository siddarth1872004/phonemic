#pragma once

#include <string>
#include <vector>

#include "app.hpp"

struct ImFont;

namespace phonemic {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* big = nullptr;  // the IP address
};

// Per-window UI state that isn't a user setting.
struct UiState {
    Fonts fonts;
    float scale = 1.0f;
    std::string pin_edit;
    bool pin_visible = false;
    bool pin_loaded = false;
    int port_edit = 4010;
    float level = 0, peak_hold = 0;
    uint64_t peak_hold_ms = 0;
    std::string qr_text;
    std::vector<bool> qr;
    int qr_size = 0;
    bool show_log = false;
    bool show_advanced = false;
};

void setup_style(float scale);
Fonts load_fonts(float scale);
void draw_ui(App& app, UiState& ui);

}  // namespace phonemic
