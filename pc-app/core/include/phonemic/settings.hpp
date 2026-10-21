// Persistent user settings (a small key=value file in the per-user config dir).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace phonemic {

// %APPDATA%\PhoneMic or $XDG_CONFIG_HOME/phonemic (default ~/.config/phonemic). Created on demand.
std::filesystem::path config_dir();

struct Settings {
    uint16_t port = 4010;              // Wi-Fi UDP and USB TCP (127.0.0.1 only)
    bool wifi_enabled = true;
    std::string passphrase;            // Wi-Fi PIN; empty = no encryption required
    bool remember_passphrase = true;
    bool usb_enabled = false;          // keep `adb reverse` set up for USB mode
    bool voice_focus = true;
    int effect = 0;
    float gain_db = 0.0f;
    bool mute = false;
    bool monitor = false;              // also play on this PC's speakers
    std::string mic_device;            // empty = auto (virtual cable if present)
    std::string monitor_device;        // empty = system default
    int latency_ms = 40;               // playout buffer target
    int theme = 0;                     // 0 OLED black, 1 dark, 2 light
    int accent = 0;                    // index into the UI's accent colours

    // Missing/invalid keys keep their defaults. Never throws.
    static Settings load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;  // atomic replace, 0600 on POSIX

    static std::filesystem::path default_file() { return config_dir() / "settings.ini"; }
};

// Exposed for tests.
std::map<std::string, std::string> parse_ini(const std::string& text);

}  // namespace phonemic
