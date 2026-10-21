#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "phonemic/denoise.hpp"
#include "phonemic/voice_fx.hpp"

namespace phonemic {

namespace {

// Palette: set by setup_style() from the theme and accent the user picked.
// OLED black is the default: true black, near-black cards with a hairline edge.
constexpr ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
ImVec4 kBg, kCard, kEdge, kAccent, kOnAccent, kGreen, kAmber, kRed, kMuted, kText, kControl, kControlHover;

struct Theme {
    const char* name;
    ImVec4 bg, card, edge, control, control_hover, text, muted, green, amber, red;
    bool light;
};
const Theme kThemes[] = {
    {"OLED black", rgb(0x00, 0x00, 0x00), rgb(0x0B, 0x0B, 0x0E), rgb(0x1C, 0x1C, 0x22), rgb(0x16, 0x16, 0x1B),
     rgb(0x22, 0x22, 0x2A), rgb(0xF2, 0xF2, 0xF5), rgb(0x84, 0x84, 0x90), rgb(0x3D, 0xDC, 0x84), rgb(0xF2, 0xA1, 0x3B),
     rgb(0xFF, 0x5C, 0x5C), false},
    {"Dark", rgb(0x14, 0x14, 0x18), rgb(0x1D, 0x1D, 0x23), rgb(0x2C, 0x2C, 0x35), rgb(0x26, 0x26, 0x2E),
     rgb(0x31, 0x31, 0x3B), rgb(0xF2, 0xF2, 0xF5), rgb(0x94, 0x94, 0xA0), rgb(0x3D, 0xDC, 0x84), rgb(0xF2, 0xA1, 0x3B),
     rgb(0xFF, 0x5C, 0x5C), false},
    {"Light", rgb(0xF4, 0xF4, 0xF7), rgb(0xFF, 0xFF, 0xFF), rgb(0xE1, 0xE1, 0xE8), rgb(0xEC, 0xEC, 0xF1),
     rgb(0xE0, 0xE0, 0xE8), rgb(0x17, 0x17, 0x1C), rgb(0x6A, 0x6A, 0x76), rgb(0x1F, 0x9D, 0x57), rgb(0xB8, 0x6E, 0x00),
     rgb(0xD6, 0x3B, 0x3B), true},
};
constexpr int kThemeCount = 3;

// Each accent has a bright shade for the dark themes and a deeper one that
// still reads on white.
struct Accent {
    const char* name;
    ImVec4 on_dark, on_light;
};
const Accent kAccents[] = {
    {"Periwinkle", rgb(0x7A, 0x8C, 0xFF), rgb(0x47, 0x57, 0xD6)},
    {"Teal", rgb(0x2F, 0xD1, 0xC0), rgb(0x0B, 0x8F, 0x82)},
    {"Violet", rgb(0xB4, 0x8C, 0xFF), rgb(0x7A, 0x4F, 0xD9)},
    {"Rose", rgb(0xFF, 0x7A, 0xA8), rgb(0xD2, 0x3F, 0x73)},
    {"Amber", rgb(0xFF, 0xB0, 0x52), rgb(0xC7, 0x6A, 0x12)},
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
    kAccent = t.light ? a.on_light : a.on_dark;
    kOnAccent = t.light ? rgb(0xFF, 0xFF, 0xFF) : rgb(0x00, 0x00, 0x00);
}

float S(UiState& ui, float v) { return v * ui.scale; }

void section_label(UiState& ui, const char* text) {
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "%s", text);
    ImGui::PopFont();
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

bool begin_card(UiState& ui, const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(ui, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(ui, 18), S(ui, 16)));
    return ImGui::BeginChild(id, ImVec2(0, 0),
                             ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}

void end_card(UiState& ui) {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, S(ui, 6)));
}

void status_dot(const ImVec4& c, float radius) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, p.y + h * 0.5f), radius,
                                                ImGui::ColorConvertFloat4ToU32(c));
    ImGui::Dummy(ImVec2(radius * 2, h));
    ImGui::SameLine();
}

bool pill(UiState& ui, const char* label, bool selected) {
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? kAccent : kControl);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kAccent : kControlHover);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kOnAccent : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(ui, 999));
    const bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return r;
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

// ---- cards ----------------------------------------------------------------------

void header(App&, UiState& ui) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(ui, 18);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + r, p.y + r);
    // The app icon (packaging/linux/phonemic.svg) in the current accent, drawn
    // from the svg's 256-unit coordinates.
    const float k = 2 * r / 240;
    auto at = [&](float x, float y) { return ImVec2(c.x + (x - 128) * k, c.y + (y - 128) * k); };
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImGui::ColorConvertFloat4ToU32(kAccent), 56 * k);
    const ImU32 w = ImGui::ColorConvertFloat4ToU32(kOnAccent);
    ImVec4 faint = kOnAccent;
    faint.w = 0.55f;
    dl->AddRectFilled(at(100, 40), at(156, 148), w, 28 * k);
    dl->PathArcTo(at(128, 116), 56 * k, 0.0f, 3.14159f, 20);
    dl->PathStroke(w, 0, 14 * k);
    dl->AddLine(at(128, 172), at(128, 200), w, 14 * k);
    dl->AddLine(at(100, 208), at(156, 208), w, 14 * k);
    for (float a0 : {-0.5236f, 3.1416f - 0.5236f}) {
        dl->PathArcTo(at(128, 94), 92 * k, a0, a0 + 1.0472f, 12);
        dl->PathStroke(ImGui::ColorConvertFloat4ToU32(faint), 0, 12 * k);
    }
    ImGui::Dummy(ImVec2(r * 2, r * 2));
    ImGui::SameLine(0, S(ui, 12));
    ImGui::BeginGroup();
    ImGui::PushFont(ui.fonts.title);
    ImGui::TextColored(kText, "PhoneMic");
    ImGui::PopFont();
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "your phone, as a microphone  ·  v%s", PHONEMIC_VERSION);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(ui, 6)));
}

void phone_card(App& app, UiState& ui) {
    const auto& s = app.snap;
    begin_card(ui, "phone");
    section_label(ui, "PHONE");
    ImGui::PushFont(ui.fonts.big);
    ImGui::TextColored(s.connected ? kText : kMuted, "%s", s.connected ? s.peer.c_str() : "Waiting for your phone");
    ImGui::PopFont();

    const uint64_t now = mono_ms();
    const bool recent_reject = s.rejected && now - s.last_reject_ms < 2500 && reject_text(s.last_reject);
    const bool data_arriving = s.last_datagram_ms && now - s.last_datagram_ms < 2000;
    ImVec4 dot = kAccent;
    std::string text;
    if (s.connected) {
        dot = kGreen;
        char buf[256];
        if (s.opus) std::snprintf(buf, sizeof buf, "Connected over %s  ·  Opus%s", via_name(s.via), s.encrypted ? "  ·  encrypted" : "");
        else
            std::snprintf(buf, sizeof buf, "Connected over %s  ·  %.1f kHz%s", via_name(s.via), s.sample_rate / 1000.0,
                          s.encrypted ? "  ·  encrypted" : "");
        text = buf;
    } else if (recent_reject) {
        dot = s.last_reject == PM_STATUS_BUSY ? kAmber : kRed;
        text = reject_text(s.last_reject);
    } else if (data_arriving && s.malformed) {
        dot = kAmber;
        text = "Data is arriving but can't be read — update the phone app.";
    } else if (app.bluetooth().connections() > 0 || app.usb().connections() > 0) {
        text = "Phone connected — tap Start in the app.";
    } else {
        text = "In the PhoneMic app pick this PC (Bluetooth, Wi-Fi or USB) and tap Start.";
    }
    status_dot(dot, S(ui, 5));
    ImGui::PushStyleColor(ImGuiCol_Text, s.connected ? kText : (dot.x == kAccent.x ? kMuted : dot));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    end_card(ui);
}

// One transport's line: a coloured dot and what it is doing.
void transport_status(UiState& ui, const ImVec4& dot, const std::string& text) {
    status_dot(dot, S(ui, 4));
    ImGui::PushFont(ui.fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, dot.x == kRed.x || dot.x == kAmber.x ? dot : kMuted);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void bluetooth_section(App& app, UiState& ui) {
    const auto& bt = app.bluetooth();
    const std::string name = bt.adapter_name();
    ImGui::TextColored(kText, "Bluetooth");
    ImVec4 dot = kMuted;
    std::string text;
    switch (bt.state()) {
        case BluetoothServer::State::Starting: text = "Starting Bluetooth…"; break;
        case BluetoothServer::State::Unavailable:
            dot = kRed;
            text = bt.detail().empty() ? "No Bluetooth on this PC." : "No Bluetooth: " + bt.detail() + ".";
            break;
        case BluetoothServer::State::PoweredOff:
            dot = kAmber;
            text = "Bluetooth is off. " + (bt.detail().empty() ? std::string("Turn it on.") : bt.detail() + ".");
            break;
        case BluetoothServer::State::Failed:
            dot = kRed;
            text = bt.detail().empty() ? "Bluetooth isn't working." : bt.detail() + ".";
            break;
        case BluetoothServer::State::Ready:
            dot = kGreen;
            text = "Ready as \"" + (name.empty() ? std::string("this PC") : name) +
                   "\". Pair the phone with this PC once in Bluetooth settings.";
            break;
    }
    transport_status(ui, dot, text);
}

void wifi_section(App& app, UiState& ui) {
    bool on = app.settings.wifi_enabled;
    if (ImGui::Checkbox("Wi-Fi", &on)) app.set_wifi_enabled(on);
    if (!on) {
        transport_status(ui, kMuted, "Off.");
        return;
    }
    if (!app.wifi_error.empty()) {
        transport_status(ui, kRed, "Couldn't start: " + app.wifi_error);
    } else if (app.wifi().running()) {
        std::string ips;
        for (const auto& a : app.addresses) ips += (ips.empty() ? "" : ", ") + a.ip;
        transport_status(ui, kGreen,
                         "Ready on port " + std::to_string(app.wifi().port()) +
                             (ips.empty() ? std::string(". No network found.") : " at " + ips + ".") +
                             " Phones on the same network find this PC by themselves.");
    }

    // The PIN: phones on Wi-Fi must seal their audio with it. Applied when the
    // field loses focus, not on every keystroke (each change wipes the keys).
    if (!ui.pin_loaded) {
        ui.pin = app.settings.passphrase;
        ui.pin_loaded = true;
    }
    ImGui::SetNextItemWidth(S(ui, 180));
    ImGui::InputTextWithHint("##pin", "PIN (optional)", &ui.pin, ImGuiInputTextFlags_Password);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.apply_passphrase(ui.pin);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With a PIN, only phones that know it can talk into this microphone over Wi-Fi.\n"
                          "Bluetooth and USB are already private links and don't need it.");
    ImGui::SameLine();
    bool remember = app.settings.remember_passphrase;
    if (ImGui::Checkbox("remember", &remember)) app.set_remember_passphrase(remember);
    ImGui::SameLine();
    int port = app.settings.port;
    ImGui::SetNextItemWidth(S(ui, 90));
    ImGui::InputInt("port", &port, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.set_port(static_cast<uint16_t>(std::clamp(port, 1024, 65535)));
}

void usb_section(App& app, UiState& ui) {
    bool on = app.settings.usb_enabled;
    if (ImGui::Checkbox("USB cable", &on)) app.set_usb_enabled(on);
    if (!on) {
        transport_status(ui, kMuted, "Off. Turn on to stream over a cable (needs adb and USB debugging).");
        return;
    }
    if (!app.usb_error.empty()) {
        transport_status(ui, kRed, "Couldn't start: " + app.usb_error);
        return;
    }
    switch (app.adb().state()) {
        case AdbBridge::State::Starting: transport_status(ui, kMuted, "Looking for adb…"); break;
        case AdbBridge::State::NoAdb:
            transport_status(ui, kAmber, "adb not found. Install Android platform-tools (or add adb to PATH).");
            break;
        case AdbBridge::State::NoDevice:
            transport_status(ui, kMuted, "Plug the phone in with USB debugging on.");
            break;
        case AdbBridge::State::Unauthorized:
            transport_status(ui, kAmber, "Allow USB debugging on the phone's screen.");
            break;
        case AdbBridge::State::Ready: {
            const std::string dev = app.adb().device();
            transport_status(ui, kGreen, "Ready" + (dev.empty() ? std::string() : " — " + dev) + ". Pick USB in the app.");
            break;
        }
    }
}

void connections_card(App& app, UiState& ui) {
    begin_card(ui, "connections");
    section_label(ui, "CONNECTIONS");
    bluetooth_section(app, ui);
    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    wifi_section(app, ui);
    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    usb_section(app, ui);
    end_card(ui);
}

void level_card(App& app, UiState& ui) {
    const auto& s = app.snap;
    begin_card(ui, "level");
    section_label(ui, app.settings.mute ? "MIC LEVEL  (muted)" : "MIC LEVEL");

    // Fast attack, slow release; plus a 1.5 s peak hold marker.
    const float target = s.connected ? s.peak : 0.0f;
    ui.level = std::max(target, ui.level * 0.85f);
    const uint64_t now = mono_ms();
    if (target >= ui.peak_hold || now - ui.peak_hold_ms > 1500) {
        ui.peak_hold = target;
        ui.peak_hold_ms = now;
    }
    // Draw on a dB scale (-60..0 dBFS) so normal speech fills a useful range.
    auto to_x = [](float lin) {
        if (lin <= 0.001f) return 0.0f;
        return std::clamp((20.0f * std::log10(lin) + 60.0f) / 60.0f, 0.0f, 1.0f);
    };
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = S(ui, 14), rr = h * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(kControl), rr);
    const float lx = to_x(ui.level) * w;
    const ImVec4 fill = ui.level > 0.95f ? kRed : (ui.level > 0.7f ? kAmber : kAccent);
    if (lx > 1) dl->AddRectFilled(p, ImVec2(p.x + std::max(lx, h), p.y + h), ImGui::ColorConvertFloat4ToU32(fill), rr);
    const float hx = to_x(ui.peak_hold) * w;
    if (hx > 2)
        dl->AddRectFilled(ImVec2(p.x + hx - 2, p.y), ImVec2(p.x + hx, p.y + h),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(kText.x, kText.y, kText.z, 0.7f)));
    ImGui::Dummy(ImVec2(w, h));

    ImGui::PushFont(ui.fonts.small);
    if (s.connected) {
        const double loss = (s.packets + s.lost) ? 100.0 * static_cast<double>(s.lost) / static_cast<double>(s.packets + s.lost) : 0.0;
        char fec[48] = "";
        if (s.opus && s.fec_recovered)
            std::snprintf(fec, sizeof fec, "  ·  %llu rebuilt by FEC", static_cast<unsigned long long>(s.fec_recovered));
        ImGui::TextColored(kMuted, "loss %.1f%%%s  ·  jitter %.1f ms  ·  buffer %.0f ms%s", loss, fec, s.jitter_ms,
                           app.mic_stats.fill_ms,
                           app.settings.voice_focus && Denoiser::available() && s.voice_prob > 0.6f ? "  ·  voice" : "");
    } else {
        ImGui::TextColored(kMuted, "no audio");
    }
    ImGui::PopFont();
    end_card(ui);
}

void voice_card(App& app, UiState& ui) {
    begin_card(ui, "voice");
    section_label(ui, "VOICE");
    if (Denoiser::available()) {
        bool vf = app.settings.voice_focus;
        if (ImGui::Checkbox("Voice Focus", &vf)) app.set_voice_focus(vf);
        ImGui::SameLine();
        ImGui::PushFont(ui.fonts.small);
        ImGui::TextColored(kMuted, "removes fans, hiss, keyboard and room noise");
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    section_label(ui, "VOICE CHANGER");
    for (int e = 0; e < kEffectCount; ++e) {
        if (e) ImGui::SameLine(0, S(ui, 6));
        // Wrap to the next line when out of room.
        const float bw = ImGui::CalcTextSize(effect_name(static_cast<Effect>(e))).x + ImGui::GetStyle().FramePadding.x * 2;
        if (e && ImGui::GetContentRegionAvail().x < bw) ImGui::NewLine();
        if (pill(ui, effect_name(static_cast<Effect>(e)), app.settings.effect == e)) app.set_effect(e);
    }

    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    section_label(ui, "VOLUME");
    float gain = app.settings.gain_db;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(ui, 110));
    if (ImGui::SliderFloat("##gain", &gain, -12.0f, 18.0f, gain >= 0 ? "+%.0f dB" : "%.0f dB")) app.set_gain_db(std::round(gain));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) app.set_gain_db(0);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, app.settings.mute ? kRed : kControl);
    if (ImGui::Button(app.settings.mute ? "Unmute" : "Mute", ImVec2(S(ui, 100), 0))) app.set_mute(!app.settings.mute);
    ImGui::PopStyleColor();
    end_card(ui);
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

void output_card(App& app, UiState& ui) {
    begin_card(ui, "output");
    section_label(ui, "MICROPHONE OUTPUT");
    if (!app.mic_error.empty()) colored_wrapped(kRed, "Audio output problem: " + app.mic_error);

    const bool busy = app.setup_busy();
    if (app.mic_is_virtual) {
        status_dot(kGreen, S(ui, 5));
        ImGui::TextColored(kText, "Microphone mode");
        if (!app.vmic.via.empty()) {
            ImGui::SameLine();
            ImGui::PushFont(ui.fonts.small);
            ImGui::TextColored(kMuted, "via %s", app.vmic.via.c_str());
            ImGui::PopFont();
        }
        ImGui::TextWrapped("In Discord / Zoom / OBS choose the microphone:");
        ImGui::TextColored(kAccent, "%s", app.vmic.mic_name.c_str());
#if defined(__linux__)
        if (app.vmic.can_persist) {
            bool keep = app.vmic.persistent;
            if (ImGui::Checkbox("Keep it when PhoneMic is closed", &keep)) app.set_persistent_mic(keep);
            ImGui::SameLine();
            ImGui::PushFont(ui.fonts.small);
            ImGui::TextColored(kMuted, "apps opened before PhoneMic list it too");
            ImGui::PopFont();
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
            ImGui::PushFont(ui.fonts.small);
            ImGui::TextColored(kMuted, app.vmic.can_rename ? "shows as \"PhoneMic Microphone\" in apps" : "back to \"CABLE Output\"");
            ImGui::PopFont();
        }
#endif
        // Apps without a microphone picker use the system default.
        if (app.default_mic == DefaultMicState::Yes) {
            muted_wrapped(ui, "It is also the default microphone, so apps without a microphone picker use it.");
        } else if (app.default_mic == DefaultMicState::No) {
            if (ImGui::SmallButton("Make it the default microphone")) app.make_default_mic();
            ImGui::SameLine();
            ImGui::PushFont(ui.fonts.small);
            ImGui::TextColored(kMuted, "for apps without a mic picker");
            ImGui::PopFont();
        }
    } else {
        status_dot(kAmber, S(ui, 5));
        ImGui::TextColored(kText, "No virtual microphone yet");
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

    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "Send audio to:");
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh list")) {
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

    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    bool mon = app.settings.monitor;
    if (ImGui::Checkbox("Listen on this PC", &mon)) app.set_monitor(mon);
    ImGui::SameLine();
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "hear yourself (use headphones)");
    ImGui::PopFont();
    if (app.settings.monitor) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (device_combo(app, "##mondev", app.settings.monitor_device, "System default output", &chosen))
            app.select_monitor_device(chosen);
        if (!app.monitor_error.empty()) colored_wrapped(kRed, app.monitor_error);
    }

    ImGui::Dummy(ImVec2(0, S(ui, 2)));
    static const int kLat[] = {20, 40, 80, 150};
    static const char* kLatNames[] = {"Lowest latency (20 ms)", "Balanced (40 ms)", "Stable (80 ms)",
                                      "Very stable (150 ms)"};
    int idx = 1;
    for (int i = 0; i < 4; ++i)
        if (kLat[i] == app.settings.latency_ms) idx = i;
    ImGui::SetNextItemWidth(S(ui, 230));
    if (ImGui::Combo("buffer", &idx, kLatNames, 4)) app.set_latency_ms(kLat[idx]);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower = less delay; higher = fewer glitches when the phone is far away or the radio is crowded.");
    end_card(ui);
}

void appearance_card(App& app, UiState& ui) {
    begin_card(ui, "appearance");
    section_label(ui, "APPEARANCE");
    for (int t = 0; t < kThemeCount; ++t) {
        if (t) ImGui::SameLine(0, S(ui, 6));
        if (pill(ui, kThemes[t].name, app.settings.theme == t)) app.set_theme(t);
    }
    // Accent swatches.
    const float r = S(ui, 11);
    auto* dl = ImGui::GetWindowDrawList();
    const bool light = kThemes[std::clamp(app.settings.theme, 0, kThemeCount - 1)].light;
    for (int a = 0; a < kAccentCount; ++a) {
        if (a) ImGui::SameLine(0, S(ui, 8));
        ImGui::PushID(a);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("accent", ImVec2(r * 2 + 4, r * 2 + 4))) app.set_accent(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kAccents[a].name);
        const ImVec2 c(p.x + r + 2, p.y + r + 2);
        dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(light ? kAccents[a].on_light : kAccents[a].on_dark));
        if (app.settings.accent == a) dl->AddCircle(c, r + 1.5f, ImGui::ColorConvertFloat4ToU32(kText), 0, 2.0f);
        ImGui::PopID();
    }
    end_card(ui);
}

void activity(App& app, UiState& ui) {
    ImGui::PushFont(ui.fonts.small);
    const auto& s = app.snap;
    ImGui::TextColored(kMuted, "%llu played  ·  %llu lost  ·  %llu received  ·  %llu rejected",
                       static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.lost),
                       static_cast<unsigned long long>(s.datagrams), static_cast<unsigned long long>(s.rejected));
    if (ImGui::CollapsingHeader("Activity")) {
        const auto lines = app.log.recent();
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) ImGui::TextColored(kMuted, "%s", it->c_str());
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
    st.WindowPadding = ImVec2(20, 18);
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(10, 8);
    st.FrameRounding = 9;
    st.GrabRounding = 9;
    st.ChildRounding = 14;
    st.PopupRounding = 10;
    st.ScrollbarRounding = 9;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = kControl;
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
    f.body = add(regular, 17);
    f.small = add(regular, 14);
    f.title = add(bold ? bold : regular, 24);
    f.big = add(bold ? bold : regular, 28);
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
    phone_card(app, ui);
    level_card(app, ui);
    connections_card(app, ui);
    voice_card(app, ui);
    output_card(app, ui);
    appearance_card(app, ui);
    activity(app, ui);
    ImGui::End();
}

}  // namespace phonemic
