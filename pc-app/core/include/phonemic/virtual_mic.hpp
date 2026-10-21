// Turning the received audio into a microphone other apps can select.
// PhoneMic uses only its own virtual microphone, "PhoneMic Microphone"; no
// third-party virtual-cable driver is used or supported:
//
//   Windows — the PhoneMic ACX driver (pc-app/driver, Windows 11 22H2+): we
//             write into the shared ring it creates.
//   Linux   — PulseAudio/PipeWire's own modules: a null sink "PhoneMic" plus a
//             remapped source "PhoneMic Microphone", created through libpulse
//             (optionally kept registered when PhoneMic is closed).
#pragma once

#include <functional>
#include <string>

#include "phonemic/util.hpp"

namespace phonemic {

struct VirtualMic {
    bool ready = false;
    // Windows driver: audio goes straight into the driver's ring (DriverFeed),
    // not through a playback device.
    bool direct_feed = false;
    std::string via;             // "PhoneMic driver", "PipeWire", "PulseAudio", ...
    std::string playback_match;  // output device to route the mic audio into
    std::string mic_name;        // what the user picks in Discord / Zoom / OBS
    std::string help;            // what to do when not ready
    // PhoneMic's own driver package found next to the app ("" = none), for the
    // Install button.
    std::string driver_package;
    // Linux: the mic can be kept registered with the sound server when PhoneMic
    // is closed (persistent), and whether it currently is.
    bool can_persist = false;
    bool persistent = false;
};

// Detect (and on Linux, create) the virtual microphone. Call once at startup,
// before opening audio devices.
VirtualMic setup_virtual_mic(EventLog* log);
// Undo what setup created (Linux: unload the pactl modules we loaded).
void teardown_virtual_mic();

#if defined(__linux__)
// Keep "PhoneMic Microphone" registered with the sound server even when
// PhoneMic isn't running (it is then created at login, like a hardware mic), so
// apps started before PhoneMic list it too. PipeWire: a pipewire-pulse.conf.d
// drop-in; PulseAudio: a marked block in ~/.config/pulse/default.pa. Undoing it
// removes exactly what was added. `config_home` overrides $XDG_CONFIG_HOME
// (tests).
bool persistent_mic_installed(const std::string& config_home = "");
bool set_persistent_mic(bool on, bool pipewire, std::string* err, const std::string& config_home = "",
                        const std::string& system_default_pa = "/etc/pulse/default.pa");
#endif

// The system's default recording device. Apps without a microphone picker
// (and "Default" in apps that have one) record whatever it is, so making
// PhoneMic the default makes it the microphone in *every* app.
enum class DefaultMicState { Unknown, Yes, No };
DefaultMicState default_mic_state(const VirtualMic& vm);
bool make_default_mic(const VirtualMic& vm, std::string* err);

// Install PhoneMic's own driver from `vm.driver_package` (Windows: runs
// install-driver.ps1 elevated, in a console that explains each step). Runs on a
// background thread; `done` gets the outcome. Re-run setup_virtual_mic after.
void install_driver_async(const VirtualMic& vm, std::function<void(bool ok, std::string message)> done);

}  // namespace phonemic
