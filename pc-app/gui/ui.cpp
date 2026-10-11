#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "phonemic/denoise.hpp"
#include "phonemic/voice_fx.hpp"
#ifdef PHONEMIC_HAVE_WEB
#include "phonemic/web_server.hpp"
#endif

namespace phonemic {

namespace {

// Palette: dark, calm, one accent.
const ImVec4 kBg = ImVec4(0x14 / 255.f, 0x16 / 255.f, 0x1B / 255.f, 1);
const ImVec4 kCard = ImVec4(0x1E / 255.f, 0x21 / 255.f, 0x29 / 255.f, 1);
const ImVec4 kAccent = ImVec4(0x4C / 255.f, 0x8B / 255.f, 0xFF / 255.f, 1);
const ImVec4 kGreen = ImVec4(0x37 / 255.f, 0xD0 / 255.f, 0x6A / 255.f, 1);
const ImVec4 kAmber = ImVec4(0xE0 / 255.f, 0x8A / 255.f, 0x2B / 255.f, 1);
const ImVec4 kRed = ImVec4(0xE0 / 255.f, 0x4B / 255.f, 0x4B / 255.f, 1);
const ImVec4 kMuted = ImVec4(0x8A / 255.f, 0x90 / 255.f, 0x9C / 255.f, 1);
const ImVec4 kText = ImVec4(0xE9 / 255.f, 0xEC / 255.f, 0xF2 / 255.f, 1);
const ImVec4 kControl = ImVec4(0x2A / 255.f, 0x2E / 255.f, 0x38 / 255.f, 1);

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
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(ui, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(ui, 16), S(ui, 14)));
    return ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

void end_card(UiState& ui) {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
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
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kAccent : ImVec4(0.2f, 0.22f, 0.27f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(ui, 999));
    const bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return r;
}

const char* reject_text(uint8_t state) {
    switch (state) {
        case PM_STATUS_PIN_REQUIRED: return "A phone is sending unencrypted audio, but this PC has a PIN. Type the same PIN in the phone app.";
        case PM_STATUS_PIN_MISSING: return "Encrypted audio is arriving. Type the phone's PIN below to unlock it.";
        case PM_STATUS_PIN_WRONG: return "Encrypted audio is arriving but the PIN doesn't match. Retype it on both ends.";
        case PM_STATUS_BUSY: return "Another device tried to connect while one is already streaming.";
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
    // A little microphone glyph.
    const ImU32 w = IM_COL32(255, 255, 255, 255);
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
    ImGui::TextColored(kMuted, "your phone, as a microphone  ·  v%s", PHONEMIC_VERSION);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(ui, 6)));
}

void connect_card(App& app, UiState& ui) {
    const auto& s = app.snap;
    begin_card(ui, "connect");
    section_label(ui, "CONNECT YOUR PHONE");

    if (!app.bind_error.empty()) {
        colored_wrapped(kRed, "Can't receive audio: " + app.bind_error);
        muted_wrapped(ui, "Usually this means PhoneMic is already running in another window, "
                          "or another program uses this port. Close it, or pick another port and Retry.");
        ImGui::SetNextItemWidth(S(ui, 110));
        ImGui::InputInt("##port", &ui.port_edit, 0);
        ImGui::SameLine();
        if (ImGui::Button("Retry")) {
            app.settings.port = static_cast<uint16_t>(std::clamp(ui.port_edit, 1024, 65535));
            app.restart_receiver();
        }
        end_card(ui);
        return;
    }

    muted_wrapped(ui, "In the PhoneMic app tap \"Find PC\" — or type this address:");
    ImGui::PushFont(ui.fonts.big);
    const std::string ip = app.addresses.empty() ? std::string("no network") : app.addresses[0].ip;
    ImGui::TextColored(kAccent, "%s", ip.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(ui, 6));
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ip.c_str());
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "port %u", app.settings.port);
    if (app.addresses.size() > 1) {
        std::string others = "also: ";
        for (size_t i = 1; i < app.addresses.size() && i < 4; ++i)
            others += app.addresses[i].ip + (i + 1 < app.addresses.size() && i < 3 ? ", " : "");
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "  ·  %s", others.c_str());
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(ui, 4)));

    // Status with real diagnostics.
    const uint64_t now = mono_ms();
    const bool recent_reject = s.rejected && now - s.last_reject_ms < 2500 && reject_text(s.last_reject);
    const bool data_arriving = s.last_datagram_ms && now - s.last_datagram_ms < 2000;
    ImVec4 dot = kMuted;
    std::string text;
    if (s.connected) {
        dot = kGreen;
        char buf[256];
        char codec[32];
        if (s.opus) std::snprintf(codec, sizeof codec, "Opus");
        else std::snprintf(codec, sizeof codec, "%.1f kHz PCM", s.sample_rate / 1000.0);
        std::snprintf(buf, sizeof buf, "Connected — %s  ·  %s  ·  %s", s.peer.c_str(),
                      s.peer_is_web ? "browser (TLS)" : (s.encrypted ? "encrypted" : "not encrypted"), codec);
        text = buf;
        if (s.sender_version == PM_VERSION_LEGACY) text += "  ·  old phone app — please update";
    } else if (recent_reject) {
        dot = s.last_reject == PM_STATUS_BUSY ? kAmber : kRed;
        text = reject_text(s.last_reject);
    } else if (s.legacy_encrypted_seen && data_arriving) {
        dot = kRed;
        text = "An old version of the phone app is sending encrypted audio. Update the phone app.";
    } else if (data_arriving && s.malformed) {
        dot = kAmber;
        text = "Data is arriving but can't be read — update the phone app.";
    } else {
        text = "Waiting for your phone…  (same Wi-Fi as this PC)";
    }
    status_dot(dot, S(ui, 5));
    ImGui::PushStyleColor(ImGuiCol_Text, s.connected ? kText : (dot.x == kMuted.x ? kMuted : dot));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();

    // PIN.
    ImGui::Dummy(ImVec2(0, S(ui, 4)));
    if (!ui.pin_loaded) {
        ui.pin_edit = app.settings.passphrase;
        ui.pin_loaded = true;
    }
    ImGui::SetNextItemWidth(S(ui, 210));
    ImGuiInputTextFlags f = ImGuiInputTextFlags_EnterReturnsTrue;
    if (!ui.pin_visible) f |= ImGuiInputTextFlags_Password;
    bool commit = ImGui::InputTextWithHint("##pin", "Security PIN (optional)", &ui.pin_edit, f);
    commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (commit) {
        std::string p = ui.pin_edit;
        while (!p.empty() && p.back() == ' ') p.pop_back();
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        if (p != app.settings.passphrase) app.apply_passphrase(p);
    }
    ImGui::SameLine();
    ImGui::Checkbox("show", &ui.pin_visible);
    ImGui::SameLine();
    bool remember = app.settings.remember_passphrase;
    if (ImGui::Checkbox("remember", &remember)) app.set_remember_passphrase(remember);
    if (app.settings.passphrase.empty())
        muted_wrapped(ui, "Set the same PIN here and in the phone app to encrypt the audio and lock out other devices.");
    else if (app.settings.passphrase.size() < 8)
        muted_wrapped(ui, "Encryption on. Tip: a longer passphrase (8+ characters) is much harder to guess than a short PIN.");
    else
        muted_wrapped(ui, "Encryption on — only phones with this passphrase can use your microphone.");
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
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(ImVec4(0.17f, 0.23f, 0.36f, 1)), rr);
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

    if (app.mic_is_virtual) {
        status_dot(kGreen, S(ui, 5));
        ImGui::TextColored(kText, "Microphone mode");
        ImGui::TextWrapped("In Discord / Zoom / OBS choose the microphone:");
        ImGui::TextColored(kAccent, "%s", app.vmic.mic_name.c_str());
    } else {
        status_dot(kAmber, S(ui, 5));
        ImGui::TextColored(kText, "No virtual microphone yet");
        if (!app.vmic.help.empty()) muted_wrapped(ui, app.vmic.help);
#ifdef _WIN32
        if (!app.vmic.ready) {
            const bool busy = app.setup_busy();
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            if (ImGui::Button("Set up microphone (VB-CABLE)")) app.install_vbcable();
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            const std::string msg = app.setup_message();
            if (!msg.empty()) colored_wrapped(busy ? kMuted : (msg.rfind("Setup failed", 0) == 0 ? kRed : kGreen), msg);
        }
#endif
    }

    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "Send audio to:");
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh list")) app.refresh_devices();
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
    static const char* kLatNames[] = {"Lowest latency (20 ms)", "Balanced (40 ms)", "Stable Wi-Fi (80 ms)",
                                      "Very stable (150 ms)"};
    int idx = 1;
    for (int i = 0; i < 4; ++i)
        if (kLat[i] == app.settings.latency_ms) idx = i;
    ImGui::SetNextItemWidth(S(ui, 230));
    if (ImGui::Combo("buffer", &idx, kLatNames, 4)) app.set_latency_ms(kLat[idx]);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower = less delay; higher = fewer glitches on busy Wi-Fi.");
    end_card(ui);
}

void browser_card(App& app, UiState& ui) {
#ifdef PHONEMIC_HAVE_WEB
    begin_card(ui, "browser");
    section_label(ui, "NO APP? USE THE PHONE'S BROWSER");
    bool on = app.settings.web_enabled;
    if (ImGui::Checkbox("Browser mic", &on)) app.set_web_enabled(on);
    ImGui::SameLine();
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "works on Android and iPhone");
    ImGui::PopFont();
    if (!app.web_error.empty()) colored_wrapped(kRed, app.web_error);
    if (app.settings.web_enabled && app.web_running()) {
        const std::string url = app.web_url();
        if (url != ui.qr_text) {
            ui.qr = qr_encode(url, &ui.qr_size);
            ui.qr_text = url;
        }
        // QR code (dark modules on a white quiet zone).
        if (ui.qr_size > 0) {
            const float cell = std::floor(S(ui, 150) / static_cast<float>(ui.qr_size + 8));
            const float side = cell * static_cast<float>(ui.qr_size + 8);
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(255, 255, 255, 255), S(ui, 6));
            for (int y = 0; y < ui.qr_size; ++y)
                for (int x = 0; x < ui.qr_size; ++x)
                    if (ui.qr[static_cast<size_t>(y) * ui.qr_size + x]) {
                        const ImVec2 a(p.x + (x + 4) * cell, p.y + (y + 4) * cell);
                        dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(0, 0, 0, 255));
                    }
            ImGui::Dummy(ImVec2(side, side));
            ImGui::SameLine(0, S(ui, 12));
        }
        ImGui::BeginGroup();
        ImGui::TextWrapped("Scan with the phone's camera to open the browser mic.");
        if (ImGui::SmallButton("Copy link")) ImGui::SetClipboardText(url.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("New code")) app.new_web_token();
        ImGui::PushFont(ui.fonts.small);
        ImGui::TextColored(kMuted, "pairing code: %s", app.settings.web_token.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::PushFont(ui.fonts.small);
        ImGui::TextColored(kAccent, "%s", url.c_str());
        muted_wrapped(ui, "The first time, the browser warns the connection isn't private (it's this PC's own "
                          "certificate): tap Advanced \u2192 Proceed.");
        ImGui::PopFont();
    }
    end_card(ui);
#else
    (void)app;
    (void)ui;
#endif
}

void usb_card(App& app, UiState& ui) {
    begin_card(ui, "usb");
    section_label(ui, "USB CABLE (ANDROID)");
    bool on = app.settings.usb_enabled;
    if (ImGui::Checkbox("Stream over USB", &on)) app.set_usb_enabled(on);
    ImGui::SameLine();
    ImGui::PushFont(ui.fonts.small);
    ImGui::TextColored(kMuted, "for bad or no Wi-Fi");
    ImGui::PopFont();
    if (app.settings.usb_enabled) {
        ImVec4 dot = kAmber;
        std::string text;
        switch (app.usb_state()) {
            case AdbBridge::State::Starting:
                text = "Looking for adb…";
                break;
            case AdbBridge::State::NoAdb:
                dot = kRed;
                text = "adb wasn't found. Install Android \"platform-tools\" (or Android Studio) and restart PhoneMic.";
                break;
            case AdbBridge::State::NoDevice:
                text = "Plug in the phone with USB debugging enabled (Settings → Developer options).";
                break;
            case AdbBridge::State::Unauthorized:
                text = "Unlock the phone and tap \"Allow USB debugging\".";
                break;
            case AdbBridge::State::Ready:
                dot = kGreen;
                text = "Ready: " + app.usb_device() + ". In the phone app turn on \"USB cable\" and tap Start.";
                break;
        }
        if (!app.usb_listening()) {
            dot = kRed;
            text = "Couldn't listen for USB connections (port in use?).";
        }
        status_dot(dot, S(ui, 5));
        ImGui::PushStyleColor(ImGuiCol_Text, dot.x == kGreen.x ? kText : dot);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
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
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = ImVec4(0.06f, 0.07f, 0.09f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.13f, 0.17f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.16f, 0.21f, 1);
    c[ImGuiCol_Button] = kControl;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.2f, 0.22f, 0.27f, 1);
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Header] = kControl;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.2f, 0.22f, 0.27f, 1);
    c[ImGuiCol_HeaderActive] = kAccent;
    c[ImGuiCol_PopupBg] = kCard;
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
    f.big = add(bold ? bold : regular, 34);
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
    connect_card(app, ui);
    level_card(app, ui);
    voice_card(app, ui);
    output_card(app, ui);
    browser_card(app, ui);
    usb_card(app, ui);
    activity(app, ui);
    ImGui::End();
}

}  // namespace phonemic
