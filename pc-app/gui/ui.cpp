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

// Palette: OLED black, near-black cards with a hairline edge, one accent.
constexpr ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
const ImVec4 kBg = rgb(0x00, 0x00, 0x00);
const ImVec4 kCard = rgb(0x0B, 0x0B, 0x0E);
const ImVec4 kEdge = rgb(0x1C, 0x1C, 0x22);
const ImVec4 kAccent = rgb(0x7A, 0x8C, 0xFF);
const ImVec4 kGreen = rgb(0x3D, 0xDC, 0x84);
const ImVec4 kAmber = rgb(0xF2, 0xA1, 0x3B);
const ImVec4 kRed = rgb(0xFF, 0x5C, 0x5C);
const ImVec4 kMuted = rgb(0x84, 0x84, 0x90);
const ImVec4 kText = rgb(0xF2, 0xF2, 0xF5);
const ImVec4 kControl = rgb(0x16, 0x16, 0x1B);
const ImVec4 kControlHover = rgb(0x22, 0x22, 0x2A);

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
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kBg : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(ui, 999));
    const bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return r;
}

const char* reject_text(uint8_t state) {
    switch (state) {
        case PM_STATUS_BUSY: return "Another phone tried to connect while one is already streaming.";
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
    dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(kAccent));
    // A little microphone glyph, cut out of the disc in black.
    const ImU32 w = IM_COL32(0, 0, 0, 255);
    dl->AddRectFilled(ImVec2(c.x - r * 0.22f, c.y - r * 0.55f), ImVec2(c.x + r * 0.22f, c.y + r * 0.15f), w, r * 0.22f);
    dl->PathArcTo(ImVec2(c.x, c.y - r * 0.05f), r * 0.38f, 0.0f, 3.14159f, 16);
    dl->PathStroke(w, 0, r * 0.09f);
    dl->AddLine(ImVec2(c.x, c.y + r * 0.33f), ImVec2(c.x, c.y + r * 0.55f), w, r * 0.09f);
    ImGui::Dummy(ImVec2(r * 2, r * 2));
    ImGui::SameLine(0, S(ui, 12));
    ImGui::BeginGroup();
    ImGui::PushFont(ui.fonts.title);
    ImGui::TextColored(kText, "PhoneMic");
    ImGui::PopFont();
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "your phone, as a Bluetooth microphone  ·  v%s", PHONEMIC_VERSION);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(ui, 6)));
}

void bluetooth_card(App& app, UiState& ui) {
    const auto& s = app.snap;
    const auto& bt = app.bluetooth();
    const auto state = bt.state();
    begin_card(ui, "bluetooth");
    section_label(ui, "BLUETOOTH");

    // The name the phone lists this PC under.
    const std::string name = bt.adapter_name();
    ImGui::PushFont(ui.fonts.big);
    ImGui::TextColored(state == BluetoothServer::State::Ready ? kText : kMuted, "%s",
                       name.empty() ? "This PC" : name.c_str());
    ImGui::PopFont();

    const uint64_t now = mono_ms();
    const bool recent_reject = s.rejected && now - s.last_reject_ms < 2500 && reject_text(s.last_reject);
    const bool data_arriving = s.last_datagram_ms && now - s.last_datagram_ms < 2000;
    ImVec4 dot = kMuted;
    std::string text;
    switch (state) {
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
            if (s.connected) {
                dot = kGreen;
                char buf[256];
                if (s.opus) std::snprintf(buf, sizeof buf, "Connected — %s  ·  Opus", s.peer.c_str());
                else std::snprintf(buf, sizeof buf, "Connected — %s  ·  %.1f kHz", s.peer.c_str(), s.sample_rate / 1000.0);
                text = buf;
            } else if (recent_reject) {
                dot = s.last_reject == PM_STATUS_BUSY ? kAmber : kRed;
                text = reject_text(s.last_reject);
            } else if (data_arriving && s.malformed) {
                dot = kAmber;
                text = "Data is arriving but can't be read — update the phone app.";
            } else if (bt.connections() > 0) {
                dot = kAccent;
                text = "Phone connected — tap Start in the app.";
            } else {
                dot = kAccent;
                text = "Ready — waiting for your phone…";
            }
            break;
    }
    status_dot(dot, S(ui, 5));
    ImGui::PushStyleColor(ImGuiCol_Text, s.connected ? kText : (dot.x == kMuted.x || dot.x == kAccent.x ? kMuted : dot));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();

    if (!s.connected) {
        ImGui::Dummy(ImVec2(0, S(ui, 2)));
        muted_wrapped(ui, "1.  Pair your phone with this PC in Bluetooth settings (once).");
        muted_wrapped(ui, "2.  In the PhoneMic app pick \"" + (name.empty() ? std::string("this PC") : name) +
                              "\" and tap Start.");
    }
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
    if (hx > 2) dl->AddRectFilled(ImVec2(p.x + hx - 2, p.y), ImVec2(p.x + hx, p.y + h), IM_COL32(255, 255, 255, 180));
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
    const bool can_install = !app.vmic.driver_package.empty();
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
        if (!app.vmic.ready && can_install) {
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            if (ImGui::Button("Install PhoneMic driver")) app.install_driver();
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        }
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

void setup_style(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
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
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    });
    const char* bold = find_font({
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeuib.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf",
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
    bluetooth_card(app, ui);
    level_card(app, ui);
    voice_card(app, ui);
    output_card(app, ui);
    activity(app, ui);
    ImGui::End();
}

}  // namespace phonemic
