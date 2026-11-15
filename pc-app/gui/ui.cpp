#include "ui.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "phonemic/denoise.hpp"
#include "phonemic/voice_fx.hpp"

namespace phonemic {

namespace {

// Studio-hardware look: graphite faceplates, LED indicators, one hot signal
// colour (the accent) reserved for "live". Same palettes as the phone app
// (phone-app/app/src/main/res/values/themes.xml).
constexpr ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
ImVec4 kBg, kCard, kEdge, kAccent, kOnAccent, kGreen, kAmber, kRed, kMuted, kText, kControl, kControlHover, kLedOff;
bool kLight = false;

struct Theme {
    const char* name;
    ImVec4 bg, card, edge, control, control_hover, text, muted, green, amber, red, led_off;
    bool light;
};
const Theme kThemes[] = {
    {"Black", rgb(0x00, 0x00, 0x00), rgb(0x0E, 0x0E, 0x10), rgb(0x22, 0x22, 0x26), rgb(0x18, 0x18, 0x1B),
     rgb(0x23, 0x23, 0x27), rgb(0xEE, 0xEE, 0xF0), rgb(0x7E, 0x7E, 0x86), rgb(0x3D, 0xDC, 0x84), rgb(0xFF, 0xB0, 0x20),
     rgb(0xFF, 0x4D, 0x4D), rgb(0x26, 0x26, 0x2B), false},
    {"Graphite", rgb(0x16, 0x16, 0x18), rgb(0x1F, 0x1F, 0x22), rgb(0x31, 0x31, 0x36), rgb(0x2A, 0x2A, 0x2E),
     rgb(0x35, 0x35, 0x3A), rgb(0xEE, 0xEE, 0xF0), rgb(0x90, 0x90, 0x98), rgb(0x3D, 0xDC, 0x84), rgb(0xFF, 0xB0, 0x20),
     rgb(0xFF, 0x4D, 0x4D), rgb(0x34, 0x34, 0x3A), false},
    {"Silver", rgb(0xD8, 0xD9, 0xDC), rgb(0xEC, 0xED, 0xEF), rgb(0xBF, 0xC1, 0xC6), rgb(0xDF, 0xE0, 0xE3),
     rgb(0xD1, 0xD3, 0xD7), rgb(0x19, 0x1A, 0x1D), rgb(0x5C, 0x5E, 0x66), rgb(0x1F, 0x9D, 0x57), rgb(0xB8, 0x6E, 0x00),
     rgb(0xD6, 0x3B, 0x3B), rgb(0xC6, 0xC8, 0xCC), true},
};
constexpr int kThemeCount = 3;

// Each accent has a bright shade for the dark themes and a deeper one that
// still reads on the silver faceplate.
struct Accent {
    const char* name;
    ImVec4 on_dark, on_light;
};
const Accent kAccents[] = {
    {"Signal", rgb(0xFF, 0x6B, 0x1A), rgb(0xD9, 0x50, 0x0B)},
    {"Teal", rgb(0x2F, 0xD1, 0xC0), rgb(0x0B, 0x8F, 0x82)},
    {"Violet", rgb(0xB4, 0x8C, 0xFF), rgb(0x7A, 0x4F, 0xD9)},
    {"Rose", rgb(0xFF, 0x5C, 0x93), rgb(0xD2, 0x3F, 0x73)},
    {"Ice", rgb(0x6F, 0xA8, 0xFF), rgb(0x2F, 0x6B, 0xD6)},
};
constexpr int kAccentCount = 5;

void set_palette(int theme, int accent) {
    const Theme& t = kThemes[std::clamp(theme, 0, kThemeCount - 1)];
    const Accent& a = kAccents[std::clamp(accent, 0, kAccentCount - 1)];
    kBg = t.bg;
    kCard = t.card;
    kEdge = t.edge;
    kControl = t.control;
    kControlHover = t.control_hover;
    kText = t.text;
    kMuted = t.muted;
    kGreen = t.green;
    kAmber = t.amber;
    kRed = t.red;
    kLedOff = t.led_off;
    kLight = t.light;
    kAccent = t.light ? a.on_light : a.on_dark;
    kOnAccent = rgb(0x00, 0x00, 0x00);
    if (t.light) kOnAccent = rgb(0xFF, 0xFF, 0xFF);
}

float S(UiState& ui, float v) { return v * ui.scale; }
ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 u32(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
bool same(const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// ---- hardware primitives --------------------------------------------------------

// Letter-spaced caps, like silkscreened panel legends. ASCII only.
float spaced_width(ImFont* f, const char* t, float track) {
    float w = 0;
    for (const char* c = t; *c; ++c) w += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, c, c + 1).x + (c[1] ? track : 0);
    return w;
}

void spaced_text(ImDrawList* dl, ImVec2 p, ImFont* f, ImU32 col, const char* t, float track) {
    for (const char* c = t; *c; ++c) {
        dl->AddText(f, f->FontSize, p, col, c, c + 1);
        p.x += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, c, c + 1).x + track;
    }
}

void legend(UiState& ui, const char* text, const ImVec4& col) {
    ImFont* f = ui.fonts.tiny;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spaced_text(ImGui::GetWindowDrawList(), p, f, u32(col), text, S(ui, 1.6f));
    ImGui::Dummy(ImVec2(spaced_width(f, text, S(ui, 1.6f)), f->FontSize));
}

// An LED: lit ones glow, dark ones sit in the panel like a dead lamp.
void led(ImDrawList* dl, ImVec2 c, float r, const ImVec4& col, bool on) {
    if (on) {
        dl->AddCircleFilled(c, r * 2.6f, u32(col, 0.10f));
        dl->AddCircleFilled(c, r * 1.7f, u32(col, 0.22f));
        dl->AddCircleFilled(c, r, u32(col));
        dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.35f, IM_COL32(255, 255, 255, 110));
    } else {
        dl->AddCircleFilled(c, r, u32(kLedOff));
        dl->AddCircle(c, r, u32(kEdge), 0, 1.0f);
    }
}

// Inline LED followed by text on the same line.
void led_inline(UiState& ui, const ImVec4& col, bool on = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight(), r = S(ui, 3.5f);
    led(ImGui::GetWindowDrawList(), ImVec2(p.x + r + S(ui, 1), p.y + h * 0.5f), r, col, on);
    ImGui::Dummy(ImVec2(r * 2 + S(ui, 2), h));
    ImGui::SameLine(0, S(ui, 8));
}

// A cross-head panel screw.
void screw(ImDrawList* dl, ImVec2 c, float r) {
    dl->AddCircleFilled(c, r, u32(kControl));
    dl->AddCircle(c, r, u32(kEdge), 0, 1.0f);
    const float k = r * 0.55f;
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), u32(kMuted, 0.45f), 1.0f);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), u32(kMuted, 0.45f), 1.0f);
}

// A rack module: faceplate, screws in the corners, a silkscreened title and an
// optional status lamp on the right of the title.
void begin_module(UiState& ui, const char* id, const char* title, const char* lamp = nullptr,
                  const ImVec4& lamp_col = ImVec4(), bool lamp_on = false) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(ui, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(ui, 22), S(ui, 14)));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
    // A faint top bevel catches the "light".
    dl->AddLine(ImVec2(wp.x + S(ui, 6), wp.y + 1), ImVec2(wp.x + ws.x - S(ui, 6), wp.y + 1),
                kLight ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 14), 1.0f);
    const float sr = S(ui, 3.2f), si = S(ui, 9);
    screw(dl, ImVec2(wp.x + si, wp.y + si), sr);
    screw(dl, ImVec2(wp.x + ws.x - si, wp.y + si), sr);
    screw(dl, ImVec2(wp.x + si, wp.y + ws.y - si), sr);
    screw(dl, ImVec2(wp.x + ws.x - si, wp.y + ws.y - si), sr);
    dl->PopClipRect();

    const float y0 = ImGui::GetCursorPosY();
    legend(ui, title, kMuted);
    if (lamp) {
        ImFont* f = ui.fonts.tiny;
        const float tw = spaced_width(f, lamp, S(ui, 1.6f));
        const float r = S(ui, 3);
        const float x = ImGui::GetWindowContentRegionMax().x - tw;
        const ImVec2 base(ImGui::GetWindowPos().x + x, ImGui::GetWindowPos().y + y0 - ImGui::GetScrollY());
        spaced_text(dl, base, f, u32(lamp_on ? lamp_col : kMuted), lamp, S(ui, 1.6f));
        led(dl, ImVec2(base.x - r - S(ui, 8), base.y + f->FontSize * 0.5f), r, lamp_col, lamp_on);
    }
    // Hairline under the title, edge to edge.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(wp.x + 1, p.y + S(ui, 3)), ImVec2(wp.x + ws.x - 1, p.y + S(ui, 3)), u32(kEdge), 1.0f);
    ImGui::Dummy(ImVec2(0, S(ui, 8)));
}

void end_module(UiState& ui) {
    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
}

// A hardware key: raised cap, a lamp in its corner that lights when engaged.
bool key(UiState& ui, const char* id, const char* label, bool on, ImVec2 size, const ImVec4* lamp = nullptr) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    auto* dl = ImGui::GetWindowDrawList();
    const float rr = S(ui, 5), d = held ? S(ui, 1) : 0;
    const ImVec2 a(p.x, p.y + d), b(p.x + size.x, p.y + size.y);
    // Shadow under the cap, then the cap.
    dl->AddRectFilled(ImVec2(p.x, p.y + S(ui, 2)), b, kLight ? IM_COL32(0, 0, 0, 40) : IM_COL32(0, 0, 0, 120), rr);
    dl->AddRectFilled(a, ImVec2(b.x, b.y - S(ui, 2) + d), u32(hovered ? kControlHover : kControl), rr);
    dl->AddRect(a, ImVec2(b.x, b.y - S(ui, 2) + d), u32(on ? ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f) : kEdge), rr,
                0, 1.0f);
    dl->AddLine(ImVec2(a.x + rr, a.y + 1), ImVec2(b.x - rr, a.y + 1),
                kLight ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 255, 255, 18), 1.0f);
    led(dl, ImVec2(a.x + S(ui, 10), a.y + S(ui, 10)), S(ui, 2.6f), lamp ? *lamp : kAccent, on);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float cy = a.y + (size.y - S(ui, 2) - ts.y) * 0.5f;
    dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, cy), u32(kText, on ? 1.0f : 0.8f), label);
    return pressed;
}

// Latching switch with an LED bar, in place of a checkbox.
bool latch(UiState& ui, const char* label, bool* v) {
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.9f;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const bool pressed = ImGui::InvisibleButton("##latch", ImVec2(w + S(ui, 10) + ts.x, std::max(h, ts.y)));
    if (pressed) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    const float y = p.y + (std::max(h, ts.y) - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), u32(hovered ? kControlHover : kControl), S(ui, 4));
    dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + w, y + h), u32(kEdge), S(ui, 4), 0, 1.0f);
    // The lamp bar slides to the "on" side.
    const float pad = S(ui, 3.5f), bw = (w - pad * 2) * 0.5f;
    const ImVec2 b0(*v ? p.x + w - pad - bw : p.x + pad, y + pad), b1(b0.x + bw, y + h - pad);
    if (*v) dl->AddRectFilled(ImVec2(b0.x - S(ui, 2), b0.y - S(ui, 2)), ImVec2(b1.x + S(ui, 2), b1.y + S(ui, 2)),
                              u32(kAccent, 0.2f), S(ui, 3));
    dl->AddRectFilled(b0, b1, u32(*v ? kAccent : kLedOff), S(ui, 2));
    dl->AddText(ImVec2(p.x + w + S(ui, 10), p.y + (std::max(h, ts.y) - ts.y) * 0.5f), u32(kText), label);
    ImGui::PopID();
    return pressed;
}

// A console fader. Double-click puts it back at `home`.
bool fader(UiState& ui, const char* id, float* v, float lo, float hi, float home, float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(ui, 34), capw = S(ui, 14);
    ImGui::InvisibleButton(id, ImVec2(width, h));
    bool changed = false;
    const float span = width - capw;
    if (ImGui::IsItemActive()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x - capw * 0.5f) / span, 0.0f, 1.0f);
        const float nv = lo + t * (hi - lo);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        *v = home;
        changed = true;
    }
    auto* dl = ImGui::GetWindowDrawList();
    auto x_of = [&](float val) { return p.x + capw * 0.5f + (val - lo) / (hi - lo) * span; };
    const float gy = p.y + h * 0.42f;
    // Scale ticks every 6 dB, a long one at home.
    for (float t = lo; t <= hi + 0.01f; t += 6.0f) {
        const float x = x_of(t), len = t == home ? S(ui, 9) : S(ui, 5);
        dl->AddLine(ImVec2(x, gy + S(ui, 7)), ImVec2(x, gy + S(ui, 7) + len), u32(kMuted, t == home ? 0.9f : 0.5f), 1.0f);
    }
    dl->AddRectFilled(ImVec2(p.x + capw * 0.5f, gy - S(ui, 2.5f)), ImVec2(p.x + width - capw * 0.5f, gy + S(ui, 2.5f)),
                      kLight ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 200), S(ui, 2));
    const float hx = x_of(home), vx = x_of(*v);
    dl->AddRectFilled(ImVec2(std::min(hx, vx), gy - S(ui, 1.5f)), ImVec2(std::max(hx, vx), gy + S(ui, 1.5f)), u32(kAccent),
                      S(ui, 1));
    // The cap: a ridged block with an index line.
    const ImVec2 c0(vx - capw * 0.5f, gy - S(ui, 12)), c1(vx + capw * 0.5f, gy + S(ui, 12));
    dl->AddRectFilled(ImVec2(c0.x, c0.y + S(ui, 2)), ImVec2(c1.x, c1.y + S(ui, 2)), IM_COL32(0, 0, 0, 90), S(ui, 3));
    dl->AddRectFilled(c0, c1, u32(ImGui::IsItemActive() || ImGui::IsItemHovered() ? kControlHover : kControl), S(ui, 3));
    dl->AddRect(c0, c1, u32(kEdge), S(ui, 3), 0, 1.0f);
    for (int i = -2; i <= 2; ++i) {
        if (i == 0) continue;
        const float yy = gy + i * S(ui, 4);
        dl->AddLine(ImVec2(c0.x + S(ui, 3), yy), ImVec2(c1.x - S(ui, 3), yy), u32(kMuted, 0.35f), 1.0f);
    }
    dl->AddLine(ImVec2(c0.x + S(ui, 2), gy), ImVec2(c1.x - S(ui, 2), gy), u32(kAccent), S(ui, 2));
    return changed;
}

// A row of equal-width readout windows: tiny legend over a monospace value.
struct Readout {
    const char* label;
    std::string value;
    ImVec4 color;
};

void readouts(UiState& ui, const std::vector<Readout>& cells) {
    if (cells.empty()) return;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float gap = S(ui, 6), w = ImGui::GetContentRegionAvail().x;
    const float cw = (w - gap * static_cast<float>(cells.size() - 1)) / static_cast<float>(cells.size());
    const float h = ui.fonts.tiny->FontSize + ui.fonts.mono->FontSize + S(ui, 16);
    for (size_t i = 0; i < cells.size(); ++i) {
        const float x = p.x + static_cast<float>(i) * (cw + gap);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + cw, p.y + h), kLight ? IM_COL32(0, 0, 0, 18) : IM_COL32(0, 0, 0, 110),
                          S(ui, 4));
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + cw, p.y + h), u32(kEdge), S(ui, 4), 0, 1.0f);
        spaced_text(dl, ImVec2(x + S(ui, 8), p.y + S(ui, 6)), ui.fonts.tiny, u32(kMuted), cells[i].label, S(ui, 1.2f));
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(x + cw - S(ui, 4), p.y + h), true);
        dl->AddText(ui.fonts.mono, ui.fonts.mono->FontSize,
                    ImVec2(x + S(ui, 8), p.y + S(ui, 9) + ui.fonts.tiny->FontSize), u32(cells[i].color),
                    cells[i].value.c_str());
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(w, h));
}

// Word-wrapped text with each line centred in `width`.
void centered_text(const std::string& text, const ImVec4& col, float width) {
    ImFont* f = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float x0 = ImGui::GetCursorPosX();
    const char* s = text.c_str();
    const char* end = s + text.size();
    while (s < end) {
        const char* e = f->CalcWordWrapPositionA(size / f->FontSize, s, end, width);
        if (e == s) e = s + 1;
        const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, s, e);
        ImGui::SetCursorPosX(x0 + std::max(0.0f, (width - sz.x) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(s, e);
        ImGui::PopStyleColor();
        s = e;
        while (s < end && (*s == ' ' || *s == '\n')) ++s;
    }
    ImGui::SetCursorPosX(x0);
}

void muted_wrapped(UiState& ui, const std::string& text) {
    ImGui::PushFont(ui.fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void colored_wrapped(const ImVec4& c, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void hint(UiState& ui, const char* text) {
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "%s", text);
    ImGui::PopFont();
}

const char* reject_text(uint8_t state) {
    switch (state) {
        case PM_STATUS_BUSY: return "Another phone tried to connect while one is already streaming.";
        case PM_STATUS_PIN_REQUIRED: return "A phone on Wi-Fi tried to connect without the PIN.";
        case PM_STATUS_PIN_MISSING: return "A phone is using a PIN, but none is set here. Enter it under Wi-Fi.";
        case PM_STATUS_PIN_WRONG: return "A phone on Wi-Fi used the wrong PIN.";
        case PM_STATUS_UNSUPPORTED: return "A device is sending audio this version can't read. Update the phone app.";
        default: return nullptr;
    }
}

float db_of(float lin) { return lin <= 0.00001f ? -100.0f : 20.0f * std::log10(lin); }
// Meter position on a -60..0 dBFS scale, so normal speech fills a useful range.
float meter_pos(float lin) { return std::clamp((db_of(lin) + 60.0f) / 60.0f, 0.0f, 1.0f); }

// ---- modules --------------------------------------------------------------------

void header(App& app, UiState& ui) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(ui, 15);
    const ImVec2 c(p.x + r, p.y + r);
    // The app icon (packaging/linux/phonemic.svg) as a lit badge, drawn from
    // the svg's 256-unit coordinates.
    const float k = 2 * r / 240;
    auto at = [&](float x, float y) { return ImVec2(c.x + (x - 128) * k, c.y + (y - 128) * k); };
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), u32(kAccent), S(ui, 6));
    const ImU32 w = u32(kOnAccent);
    dl->AddRectFilled(at(100, 40), at(156, 148), w, 28 * k);
    dl->PathArcTo(at(128, 116), 56 * k, 0.0f, 3.14159f, 20);
    dl->PathStroke(w, 0, 14 * k);
    dl->AddLine(at(128, 172), at(128, 200), w, 14 * k);
    dl->AddLine(at(100, 208), at(156, 208), w, 14 * k);

    const float tx = p.x + r * 2 + S(ui, 12);
    spaced_text(dl, ImVec2(tx, p.y + S(ui, 1)), ui.fonts.title, u32(kText), "PHONEMIC", S(ui, 3));
    char ver[48];
    std::snprintf(ver, sizeof ver, "MIC LINK  /  V%s", PHONEMIC_VERSION);
    spaced_text(dl, ImVec2(tx + S(ui, 1), p.y + ui.fonts.title->FontSize + S(ui, 3)), ui.fonts.tiny, u32(kMuted), ver,
                S(ui, 1.6f));

    // Tally light: lit only while audio is actually going out.
    const auto& s = app.snap;
    const bool live = s.connected && !app.settings.mute;
    const char* tally = live ? "ON AIR" : (s.connected ? "MUTED" : "STANDBY");
    const ImVec4 tc = live ? kAccent : (s.connected ? kRed : kMuted);
    const float tw = spaced_width(ui.fonts.tiny, tally, S(ui, 2)) + S(ui, 22);
    const float th = S(ui, 24);
    const float right = p.x + ImGui::GetContentRegionAvail().x;
    const ImVec2 t0(right - tw, p.y + r - th * 0.5f), t1(right, p.y + r + th * 0.5f);
    if (live) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
        dl->AddRectFilled(ImVec2(t0.x - S(ui, 4), t0.y - S(ui, 4)), ImVec2(t1.x + S(ui, 4), t1.y + S(ui, 4)),
                          u32(tc, 0.10f + 0.10f * pulse), S(ui, 7));
        dl->AddRectFilled(t0, t1, u32(tc), S(ui, 4));
    } else {
        dl->AddRect(t0, t1, u32(tc, 0.8f), S(ui, 4), 0, 1.0f);
    }
    spaced_text(dl, ImVec2(t0.x + S(ui, 11), p.y + r - ui.fonts.tiny->FontSize * 0.5f), ui.fonts.tiny,
                u32(live ? kOnAccent : tc), tally, S(ui, 2));
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, r * 2 + S(ui, 8)));
}

// The ring meter, phone status and the stream's numbers.
void monitor_module(App& app, UiState& ui) {
    const auto& s = app.snap;
    begin_module(ui, "monitor", "MONITOR", s.connected ? "SIGNAL" : "NO SIGNAL", kGreen, s.connected);

    // Fast attack, slow release; plus a 1.5 s peak hold.
    const float target = s.connected ? s.peak : 0.0f;
    ui.level = std::max(target, ui.level * 0.85f);
    const uint64_t now = mono_ms();
    if (target >= ui.peak_hold || now - ui.peak_hold_ms > 1500) {
        ui.peak_hold = target;
        ui.peak_hold_ms = now;
    }

    auto* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float R = std::min(avail * 0.34f, S(ui, 112));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + avail * 0.5f, p.y + R + S(ui, 6));
    const float t = static_cast<float>(ImGui::GetTime());
    const bool muted = app.settings.mute;

    // 270 degrees of LED segments, open at the bottom.
    constexpr int kTicks = 44;
    constexpr float kA0 = 0.75f * 3.14159265f, kSweep = 1.5f * 3.14159265f;
    const float lvl = meter_pos(ui.level), hold = meter_pos(ui.peak_hold);
    const int hold_i = static_cast<int>(hold * kTicks);
    for (int i = 0; i < kTicks; ++i) {
        const float f = (static_cast<float>(i) + 0.5f) / kTicks;
        const float a = kA0 + f * kSweep;
        const ImVec2 dir(std::cos(a), std::sin(a));
        const float r0 = R * 0.80f, r1 = R * (i % 4 == 0 ? 1.0f : 0.95f);
        ImVec4 col = kLedOff;
        float alpha = 1.0f;
        if (s.connected && !muted && f <= lvl) col = f > 0.9f ? kRed : (f > 0.75f ? kAmber : kAccent);
        else if (s.connected && !muted && hold > 0.02f && i == std::min(hold_i, kTicks - 1)) col = kText;
        else if (!s.connected) {
            // Idle: a slow scanner sweeps the ring.
            const float ph = std::fmod(t * 0.35f, 1.0f);
            const float d = std::fabs(f - ph);
            const float g = std::max(0.0f, 1.0f - d * 9.0f);
            if (g > 0) {
                col = kMuted;
                alpha = 0.25f + 0.6f * g;
            }
        }
        dl->AddLine(ImVec2(c.x + dir.x * r0, c.y + dir.y * r0), ImVec2(c.x + dir.x * r1, c.y + dir.y * r1),
                    u32(col, alpha), S(ui, 3.2f));
    }
    // Scale legends at the ends of the arc.
    {
        ImFont* f = ui.fonts.tiny;
        const ImVec2 la(c.x + std::cos(kA0) * R * 0.92f, c.y + std::sin(kA0) * R * 0.92f);
        const ImVec2 lb(c.x + std::cos(kA0 + kSweep) * R * 0.92f, c.y + std::sin(kA0 + kSweep) * R * 0.92f);
        dl->AddText(f, f->FontSize, ImVec2(la.x - S(ui, 8), la.y + S(ui, 6)), u32(kMuted), "-60");
        dl->AddText(f, f->FontSize, ImVec2(lb.x - S(ui, 2), lb.y + S(ui, 6)), u32(kMuted), "0");
    }

    // The centre: a knob-like disc that is also the mute button.
    const float rd = R * 0.64f;
    ImGui::SetCursorScreenPos(ImVec2(c.x - rd, c.y - rd));
    ImGui::BeginDisabled(!s.connected);
    if (ImGui::InvisibleButton("##ringmute", ImVec2(rd * 2, rd * 2))) app.set_mute(!muted);
    ImGui::EndDisabled();
    const bool hov = ImGui::IsItemHovered() && s.connected;
    if (hov) ImGui::SetTooltip(muted ? "Unmute" : "Mute");
    if (s.connected && !muted) {
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);
        dl->AddCircle(c, rd + S(ui, 4), u32(kAccent, 0.18f + 0.22f * pulse * std::max(lvl, 0.3f)), 64, S(ui, 2));
    }
    dl->AddCircleFilled(ImVec2(c.x, c.y + S(ui, 3)), rd, kLight ? IM_COL32(0, 0, 0, 40) : IM_COL32(0, 0, 0, 140), 64);
    dl->AddCircleFilled(c, rd, u32(hov ? kControlHover : kControl), 64);
    dl->AddCircle(c, rd, u32(kEdge), 64, 1.0f);
    dl->AddCircle(c, rd - S(ui, 6), u32(kEdge, 0.7f), 64, 1.0f);
    // Top highlight arc on the disc.
    dl->PathArcTo(c, rd - S(ui, 1), -2.6f, -0.55f, 32);
    dl->PathStroke(kLight ? IM_COL32(255, 255, 255, 190) : IM_COL32(255, 255, 255, 22), 0, S(ui, 1.5f));

    const char* big = !s.connected ? "STANDBY" : (muted ? "MUTED" : "LIVE");
    const ImVec4 bc = !s.connected ? kMuted : (muted ? kRed : kAccent);
    const float bw = spaced_width(ui.fonts.title, big, S(ui, 3));
    spaced_text(dl, ImVec2(c.x - bw * 0.5f, c.y - ui.fonts.title->FontSize * 0.85f), ui.fonts.title, u32(bc), big, S(ui, 3));
    char db[32];
    if (s.connected && !muted && ui.level > 0.001f) std::snprintf(db, sizeof db, "%+.0f dBFS", db_of(ui.level));
    else std::snprintf(db, sizeof db, "-- dBFS");
    const ImVec2 dsz = ui.fonts.mono->CalcTextSizeA(ui.fonts.mono->FontSize, FLT_MAX, 0, db);
    dl->AddText(ui.fonts.mono, ui.fonts.mono->FontSize, ImVec2(c.x - dsz.x * 0.5f, c.y + S(ui, 4)), u32(kMuted), db);
    if (s.connected) {
        const char* act = muted ? "CLICK TO UNMUTE" : "CLICK TO MUTE";
        const float aw = spaced_width(ui.fonts.tiny, act, S(ui, 1.2f));
        spaced_text(dl, ImVec2(c.x - aw * 0.5f, c.y + S(ui, 8) + dsz.y), ui.fonts.tiny, u32(kMuted, hov ? 1.0f : 0.6f), act,
                    S(ui, 1.2f));
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(avail, R * 2 + S(ui, 4)));

    // Who is on the other end, and what is going on.
    const bool recent_reject = s.rejected && now - s.last_reject_ms < 2500 && reject_text(s.last_reject);
    const bool data_arriving = s.last_datagram_ms && now - s.last_datagram_ms < 2000;
    ImVec4 tone = kMuted;
    std::string text;
    if (s.connected) {
        tone = kGreen;
        text = std::string("Connected over ") + via_name(s.via) + (s.encrypted ? "  ·  encrypted" : "");
    } else if (recent_reject) {
        tone = s.last_reject == PM_STATUS_BUSY ? kAmber : kRed;
        text = reject_text(s.last_reject);
    } else if (data_arriving && s.malformed) {
        tone = kAmber;
        text = "Data is arriving but can't be read — update the phone app.";
    } else if (app.bluetooth().connections() > 0 || app.usb().connections() > 0) {
        text = "Phone connected — tap Start in the app.";
    } else {
        text = "In the PhoneMic app pick this PC (Bluetooth, Wi-Fi or USB) and tap Start.";
    }
    ImGui::PushFont(ui.fonts.big);
    centered_text(s.connected ? s.peer : "Waiting for your phone", s.connected ? kText : kMuted, avail);
    ImGui::PopFont();
    ImGui::PushFont(ui.fonts.small);
    centered_text(text, same(tone, kGreen) ? kMuted : tone, avail);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(ui, 6)));

    std::vector<Readout> cells;
    char buf[64];
    if (s.connected) {
        const double loss =
            (s.packets + s.lost) ? 100.0 * static_cast<double>(s.lost) / static_cast<double>(s.packets + s.lost) : 0.0;
        if (s.opus) std::snprintf(buf, sizeof buf, "OPUS");
        else std::snprintf(buf, sizeof buf, "PCM %.1fk", s.sample_rate / 1000.0);
        cells.push_back({"CODEC", buf, kText});
        std::snprintf(buf, sizeof buf, "%.1f%%", loss);
        cells.push_back({"LOSS", buf, loss > 2.0 ? kAmber : kText});
        std::snprintf(buf, sizeof buf, "%.1fms", s.jitter_ms);
        cells.push_back({"JITTER", buf, s.jitter_ms > 20 ? kAmber : kText});
        std::snprintf(buf, sizeof buf, "%.0fms", app.mic_stats.fill_ms);
        cells.push_back({"BUFFER", buf, kText});
    } else {
        cells.push_back({"CODEC", "--", kMuted});
        cells.push_back({"LOSS", "--", kMuted});
        cells.push_back({"JITTER", "--", kMuted});
        cells.push_back({"BUFFER", "--", kMuted});
    }
    readouts(ui, cells);
    if (s.connected && (s.opus && s.fec_recovered)) {
        std::snprintf(buf, sizeof buf, "%llu lost frames rebuilt by FEC", static_cast<unsigned long long>(s.fec_recovered));
        hint(ui, buf);
    }
    end_module(ui);
}

// One transport: lamp, name, and what it is doing.
void transport_row(UiState& ui, const char* name, const ImVec4& lamp, bool lit, const std::string& text, bool* toggle,
                   bool* toggled) {
    led_inline(ui, lamp, lit);
    ImGui::PushFont(ui.fonts.mono);
    ImGui::TextColored(kText, "%s", name);
    ImGui::PopFont();
    if (toggle) {
        const float kw = S(ui, 62), kh = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - kw);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(ui, 3));
        ImGui::PushFont(ui.fonts.small);
        if (key(ui, name, *toggle ? "ON" : "OFF", *toggle, ImVec2(kw, kh))) {
            *toggle = !*toggle;
            *toggled = true;
        }
        ImGui::PopFont();
    }
    ImGui::Indent(S(ui, 17));
    ImGui::PushFont(ui.fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, same(lamp, kRed) || same(lamp, kAmber) ? lamp : kMuted);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Unindent(S(ui, 17));
}

void bluetooth_section(App& app, UiState& ui) {
    const auto& bt = app.bluetooth();
    const std::string name = bt.adapter_name();
    ImVec4 lamp = kMuted;
    bool lit = false;
    std::string text;
    switch (bt.state()) {
        case BluetoothServer::State::Starting: text = "Starting Bluetooth…"; break;
        case BluetoothServer::State::Unavailable:
            lamp = kRed;
            lit = true;
            text = bt.detail().empty() ? "No Bluetooth on this PC." : "No Bluetooth: " + bt.detail() + ".";
            break;
        case BluetoothServer::State::PoweredOff:
            lamp = kAmber;
            lit = true;
            text = "Bluetooth is off. " + (bt.detail().empty() ? std::string("Turn it on.") : bt.detail() + ".");
            break;
        case BluetoothServer::State::Failed:
            lamp = kRed;
            lit = true;
            text = bt.detail().empty() ? "Bluetooth isn't working." : bt.detail() + ".";
            break;
        case BluetoothServer::State::Ready:
            lamp = kGreen;
            lit = true;
            text = "Ready as \"" + (name.empty() ? std::string("this PC") : name) +
                   "\". Pair the phone with this PC once in Bluetooth settings.";
            break;
    }
    transport_row(ui, "BLUETOOTH", lamp, lit, text, nullptr, nullptr);
}

void wifi_section(App& app, UiState& ui) {
    bool on = app.settings.wifi_enabled, toggled = false;
    ImVec4 lamp = kMuted;
    bool lit = false;
    std::string text = "Off.";
    if (on && !app.wifi_error.empty()) {
        lamp = kRed;
        lit = true;
        text = "Couldn't start: " + app.wifi_error;
    } else if (on && app.wifi().running()) {
        std::string ips;
        for (const auto& a : app.addresses) ips += (ips.empty() ? "" : ", ") + a.ip;
        lamp = kGreen;
        lit = true;
        text = "Ready on port " + std::to_string(app.wifi().port()) +
               (ips.empty() ? std::string(". No network found.") : " at " + ips + ".") +
               " Phones on the same network find this PC by themselves.";
    } else if (on) {
        text = "Starting…";
    }
    transport_row(ui, "WI-FI", lamp, lit, text, &on, &toggled);
    if (toggled) app.set_wifi_enabled(on);
    if (!app.settings.wifi_enabled) return;

    // The PIN: phones on Wi-Fi must seal their audio with it. Applied when the
    // field loses focus, not on every keystroke (each change wipes the keys).
    if (!ui.pin_loaded) {
        ui.pin = app.settings.passphrase;
        ui.pin_loaded = true;
    }
    ImGui::Indent(S(ui, 17));
    ImGui::PushFont(ui.fonts.mono);
    ImGui::SetNextItemWidth(S(ui, 150));
    ImGui::InputTextWithHint("##pin", "PIN", &ui.pin, ImGuiInputTextFlags_Password);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.apply_passphrase(ui.pin);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With a PIN, only phones that know it can talk into this microphone over Wi-Fi.\n"
                          "Bluetooth and USB are already private links and don't need it.");
    ImGui::SameLine();
    int port = app.settings.port;
    ImGui::SetNextItemWidth(S(ui, 76));
    ImGui::InputInt("##port", &port, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.set_port(static_cast<uint16_t>(std::clamp(port, 1024, 65535)));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Port");
    ImGui::PopFont();
    ImGui::SameLine();
    bool remember = app.settings.remember_passphrase;
    if (latch(ui, "Remember PIN", &remember)) app.set_remember_passphrase(remember);
    ImGui::Unindent(S(ui, 17));
}

void usb_section(App& app, UiState& ui) {
    bool on = app.settings.usb_enabled, toggled = false;
    ImVec4 lamp = kMuted;
    bool lit = false;
    std::string text = "Off. Turn on to stream over a cable (needs adb and USB debugging).";
    if (on && !app.usb_error.empty()) {
        lamp = kRed;
        lit = true;
        text = "Couldn't start: " + app.usb_error;
    } else if (on) {
        switch (app.adb().state()) {
            case AdbBridge::State::Starting: text = "Looking for adb…"; break;
            case AdbBridge::State::NoAdb:
                lamp = kAmber;
                lit = true;
                text = "adb not found. Install Android platform-tools (or add adb to PATH).";
                break;
            case AdbBridge::State::NoDevice: text = "Plug the phone in with USB debugging on."; break;
            case AdbBridge::State::Unauthorized:
                lamp = kAmber;
                lit = true;
                text = "Allow USB debugging on the phone's screen.";
                break;
            case AdbBridge::State::Ready: {
                const std::string dev = app.adb().device();
                lamp = kGreen;
                lit = true;
                text = "Ready" + (dev.empty() ? std::string() : " — " + dev) + ". Pick USB in the app.";
                break;
            }
        }
    }
    transport_row(ui, "USB", lamp, lit, text, &on, &toggled);
    if (toggled) app.set_usb_enabled(on);
}

void inputs_module(App& app, UiState& ui) {
    begin_module(ui, "inputs", "INPUTS");
    bluetooth_section(app, ui);
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    wifi_section(app, ui);
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    usb_section(app, ui);
    end_module(ui);
}

void voice_module(App& app, UiState& ui) {
    const bool vf_ok = Denoiser::available();
    const bool vf = vf_ok && app.settings.voice_focus;
    begin_module(ui, "voice", "CHANNEL", vf ? "VOICE FOCUS" : nullptr, kGreen,
                 vf && app.snap.connected && app.snap.voice_prob > 0.6f);

    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = S(ui, 8), kh = S(ui, 44);
    if (vf_ok) {
        if (key(ui, "##vf", "VOICE FOCUS", vf, ImVec2(S(ui, 150), kh))) app.set_voice_focus(!app.settings.voice_focus);
        ImGui::SameLine(0, S(ui, 12));
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(ui, 2)));
        ImGui::PushFont(ui.fonts.small);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - S(ui, 162));
        ImGui::TextColored(kMuted, "Removes fans, hiss, keyboard and room noise. Soundboard clips skip it.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(ui, 6)));
    }

    legend(ui, "VOICE CHANGER", kMuted);
    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    constexpr int kPerRow = 3;
    const float kw = (avail - gap * (kPerRow - 1)) / kPerRow;
    ImGui::PushFont(ui.fonts.small);
    for (int e = 0; e < kEffectCount; ++e) {
        if (e % kPerRow) ImGui::SameLine(0, gap);
        ImGui::PushID(e);
        if (key(ui, "##fx", effect_name(static_cast<Effect>(e)), app.settings.effect == e, ImVec2(kw, S(ui, 38))))
            app.set_effect(e);
        ImGui::PopID();
    }
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, S(ui, 6)));
    legend(ui, "OUTPUT LEVEL", kMuted);
    const float mw = S(ui, 84), vw = S(ui, 64);
    float gain = app.settings.gain_db;
    if (fader(ui, "##gain", &gain, -12.0f, 18.0f, 0.0f, avail - mw - vw - gap * 2)) app.set_gain_db(std::round(gain));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set, double-click for 0 dB");
    ImGui::SameLine(0, gap);
    {
        char g[16];
        std::snprintf(g, sizeof g, app.settings.gain_db >= 0 ? "+%.0f dB" : "%.0f dB", app.settings.gain_db);
        ImGui::PushFont(ui.fonts.mono);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(ui, 6));
        ImGui::TextColored(kText, "%s", g);
        ImGui::PopFont();
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - mw);
    ImGui::PushFont(ui.fonts.small);
    if (key(ui, "##mute", "MUTE", app.settings.mute, ImVec2(mw, S(ui, 34)), &kRed)) app.set_mute(!app.settings.mute);
    ImGui::PopFont();
    end_module(ui);
}

bool device_combo(App& app, const char* id, const std::string& current, const char* auto_label, std::string* chosen) {
    bool changed = false;
    const std::string preview = current.empty() ? std::string(auto_label) : current;
    if (ImGui::BeginCombo(id, preview.c_str())) {
        if (ImGui::Selectable(auto_label, current.empty())) {
            *chosen = "";
            changed = true;
        }
        for (const auto& d : app.devices) {
            if (ImGui::Selectable(d.name.c_str(), d.name == current)) {
                *chosen = d.name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void output_module(App& app, UiState& ui) {
    begin_module(ui, "output", "OUTPUT", app.mic_is_virtual ? "VIRTUAL MIC" : "NO VIRTUAL MIC",
                 app.mic_is_virtual ? kGreen : kAmber, true);
    if (!app.mic_error.empty()) colored_wrapped(kRed, "Audio output problem: " + app.mic_error);

    const bool busy = app.setup_busy();
    if (app.mic_is_virtual) {
        ImGui::PushFont(ui.fonts.small);
        ImGui::TextColored(kMuted, "In Discord / Zoom / OBS choose the microphone%s",
                           app.vmic.via.empty() ? ":" : "");
        if (!app.vmic.via.empty()) {
            ImGui::SameLine(0, 0);
            ImGui::TextColored(kMuted, " (via %s):", app.vmic.via.c_str());
        }
        ImGui::PopFont();
        // The name to pick, on a lit display strip.
        {
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushFont(ui.fonts.mono);
            const ImVec2 ts = ImGui::CalcTextSize(app.vmic.mic_name.c_str());
            const float h = ts.y + S(ui, 12), w = ImGui::GetContentRegionAvail().x;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kLight ? IM_COL32(0, 0, 0, 18) : IM_COL32(0, 0, 0, 110), S(ui, 4));
            dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(kEdge), S(ui, 4), 0, 1.0f);
            dl->AddText(ImVec2(p.x + S(ui, 10), p.y + S(ui, 6)), u32(kAccent), app.vmic.mic_name.c_str());
            ImGui::Dummy(ImVec2(w, h));
            ImGui::PopFont();
        }
#if defined(__linux__)
        if (app.vmic.can_persist) {
            bool keep = app.vmic.persistent;
            if (latch(ui, "Keep it when PhoneMic is closed", &keep)) app.set_persistent_mic(keep);
            muted_wrapped(ui, "Apps opened before PhoneMic list it too.");
        }
#endif
#ifdef _WIN32
        // VB-CABLE's endpoints are called "CABLE Output"/"CABLE Input" until renamed.
        if (app.vmic.via == "VB-CABLE") {
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(app.vmic.can_rename ? "Rename to PhoneMic" : "Restore VB-CABLE names"))
                app.rename_cable(app.vmic.can_rename);
            ImGui::EndDisabled();
            ImGui::SameLine();
            hint(ui, app.vmic.can_rename ? "shows as \"PhoneMic Microphone\" in apps" : "back to \"CABLE Output\"");
        }
#endif
        // Apps without a microphone picker use the system default.
        if (app.default_mic == DefaultMicState::Yes) {
            muted_wrapped(ui, "It is also the default microphone, so apps without a microphone picker use it.");
        } else if (app.default_mic == DefaultMicState::No) {
            if (ImGui::SmallButton("Make it the default microphone")) app.make_default_mic();
            ImGui::SameLine();
            hint(ui, "for apps without a mic picker");
        }
    } else {
        if (!app.vmic.help.empty()) muted_wrapped(ui, app.vmic.help);
#ifdef _WIN32
        if (!app.vmic.ready && app.vmic.can_install) {
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
            ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
            if (ImGui::Button("Install VB-CABLE")) app.install_vbcable();
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
        }
#endif
    }
    {
        const std::string msg = app.setup_message();
        if (!msg.empty()) colored_wrapped(busy ? kMuted : (msg.rfind("Setup failed", 0) == 0 ? kRed : kGreen), msg);
    }

    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    legend(ui, "ROUTE TO", kMuted);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("REFRESH").x - S(ui, 12));
    ImGui::PushFont(ui.fonts.small);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(ui, 3));
    if (ImGui::SmallButton("REFRESH")) {
        // Look for the virtual mic again only if we don't have our own yet
        // (re-creating it would cut off apps that are recording it).
        if (!app.vmic.ready) app.redetect_virtual_mic();
        else app.refresh_devices();
    }
    ImGui::PopFont();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    std::string chosen;
    if (device_combo(app, "##micdev", app.settings.mic_device,
                     app.vmic.ready ? "Automatic (virtual microphone)" : "Automatic (default output)", &chosen))
        app.select_mic_device(chosen);

    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    bool mon = app.settings.monitor;
    if (latch(ui, "Listen on this PC", &mon)) app.set_monitor(mon);
    ImGui::SameLine();
    hint(ui, "hear yourself (use headphones)");
    if (app.settings.monitor) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (device_combo(app, "##mondev", app.settings.monitor_device, "System default output", &chosen))
            app.select_monitor_device(chosen);
        if (!app.monitor_error.empty()) colored_wrapped(kRed, app.monitor_error);
    }

    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    legend(ui, "BUFFER", kMuted);
    ImGui::Dummy(ImVec2(0, S(ui, 1)));
    static const int kLat[] = {20, 40, 80, 150};
    static const char* kLatNames[] = {"20 ms", "40 ms", "80 ms", "150 ms"};
    static const char* kLatTips[] = {"Lowest latency", "Balanced", "Stable", "Very stable"};
    const float gap = S(ui, 8);
    const float bw = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
    ImGui::PushFont(ui.fonts.small);
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        if (key(ui, "##lat", kLatNames[i], app.settings.latency_ms == kLat[i], ImVec2(bw, S(ui, 34))))
            app.set_latency_ms(kLat[i]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s. Lower = less delay; higher = fewer glitches when the phone is far away or the "
                              "radio is crowded.",
                              kLatTips[i]);
        ImGui::PopID();
    }
    ImGui::PopFont();
    end_module(ui);
}

void appearance_module(App& app, UiState& ui) {
    begin_module(ui, "appearance", "FACEPLATE");
    const float gap = S(ui, 8);
    const float bw = S(ui, 96);
    ImGui::PushFont(ui.fonts.small);
    for (int t = 0; t < kThemeCount; ++t) {
        if (t) ImGui::SameLine(0, gap);
        ImGui::PushID(t);
        if (key(ui, "##theme", kThemes[t].name, app.settings.theme == t, ImVec2(bw, S(ui, 34)))) app.set_theme(t);
        ImGui::PopID();
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    // Accent lamps.
    const float r = S(ui, 9);
    auto* dl = ImGui::GetWindowDrawList();
    for (int a = 0; a < kAccentCount; ++a) {
        if (a) ImGui::SameLine(0, S(ui, 12));
        ImGui::PushID(a);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("accent", ImVec2(r * 2 + S(ui, 8), r * 2 + S(ui, 8)))) app.set_accent(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kAccents[a].name);
        const ImVec2 c(p.x + r + S(ui, 4), p.y + r + S(ui, 4));
        const ImVec4 col = kLight ? kAccents[a].on_light : kAccents[a].on_dark;
        dl->AddCircleFilled(c, r + S(ui, 3), u32(kControl));
        dl->AddCircle(c, r + S(ui, 3), u32(app.settings.accent == a ? kText : kEdge), 0, app.settings.accent == a ? 2.0f : 1.0f);
        led(dl, c, r * 0.62f, col, true);
        ImGui::PopID();
    }
    end_module(ui);
}

void activity(App& app, UiState& ui) {
    const auto& s = app.snap;
    ImGui::PushFont(ui.fonts.mono);
    char buf[160];
    std::snprintf(buf, sizeof buf, "PLAYED %llu  LOST %llu  RECV %llu  REJ %llu", static_cast<unsigned long long>(s.packets),
                  static_cast<unsigned long long>(s.lost), static_cast<unsigned long long>(s.datagrams),
                  static_cast<unsigned long long>(s.rejected));
    ImGui::PushFont(ui.fonts.tiny);
    ImGui::TextColored(kMuted, "%s", buf);
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::PushFont(ui.fonts.small);
    if (ImGui::CollapsingHeader("Activity log")) {
        const auto lines = app.log.recent();
        ImGui::PushFont(ui.fonts.tiny);
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) ImGui::TextColored(kMuted, "%s", it->c_str());
        ImGui::PopFont();
    }
    ImGui::PopFont();
}

const char* find_font(const std::vector<const char*>& candidates) {
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return nullptr;
}

}  // namespace

void setup_style(float scale, int theme, int accent) {
    set_palette(theme, accent);
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    if (kThemes[std::clamp(theme, 0, kThemeCount - 1)].light) ImGui::StyleColorsLight(&st);
    else ImGui::StyleColorsDark(&st);
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 7);
    st.FrameRounding = 4;
    st.GrabRounding = 3;
    st.GrabMinSize = 14;
    st.ChildRounding = 6;
    st.PopupRounding = 6;
    st.ScrollbarRounding = 3;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FrameBorderSize = 1;
    st.PopupBorderSize = 1;
    st.ScrollbarSize = 8;
    ImVec4* c = st.Colors;
    const ImVec4 well = kLight ? ImVec4(0, 0, 0, 0.07f) : ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = well;
    c[ImGuiCol_FrameBgHovered] = kControlHover;
    c[ImGuiCol_FrameBgActive] = kControlHover;
    c[ImGuiCol_Button] = kControl;
    c[ImGuiCol_ButtonHovered] = kControlHover;
    c[ImGuiCol_ButtonActive] = kEdge;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Header] = kControl;
    c[ImGuiCol_HeaderHovered] = kControlHover;
    c[ImGuiCol_HeaderActive] = kEdge;
    c[ImGuiCol_PopupBg] = kCard;
    c[ImGuiCol_Border] = kEdge;
    c[ImGuiCol_Separator] = kEdge;
    c[ImGuiCol_ScrollbarGrab] = kControlHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kEdge;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.4f);
    c[ImGuiCol_ScrollbarBg] = kBg;
    c[ImGuiCol_NavHighlight] = kAccent;
    st.ScaleAllSizes(scale);
}

Fonts load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const char* regular = find_font({
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    });
    const char* bold = find_font({
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeuib.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
#endif
    });
    // Readouts and legends: a monospace face, like an instrument display.
    const char* mono = find_font({
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\CascadiaMono.ttf", "C:\\Windows\\Fonts\\consola.ttf",
#else
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
#endif
    });
    // Latin + general punctuation (dashes, quotes, bullets, ellipsis) + arrows.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0};
    auto add = [&](const char* file, float px) -> ImFont* {
        if (file) {
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(file, px * scale, nullptr, ranges)) return f;
        }
        ImFontConfig cfg;
        cfg.SizePixels = px * scale;
        return io.Fonts->AddFontDefault(&cfg);
    };
    Fonts f;
    f.body = add(regular, 16);
    f.small = add(regular, 13.5f);
    f.title = add(bold ? bold : regular, 21);
    f.big = add(bold ? bold : regular, 22);
    f.mono = add(mono, 14.5f);
    f.tiny = add(mono ? mono : regular, 11);
    return f;
}

void draw_ui(App& app, UiState& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("PhoneMic", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);  // keeps the scrollbar for small windows
    header(app, ui);
    monitor_module(app, ui);
    voice_module(app, ui);
    inputs_module(app, ui);
    output_module(app, ui);
    appearance_module(app, ui);
    activity(app, ui);
    ImGui::End();
}

}  // namespace phonemic
