// Turning the received audio into a microphone other apps can select.
// PhoneMic ships its own virtual microphone on every desktop OS:
//
//   Windows — the PhoneMic ACX driver (pc-app/driver): we write into the shared
//             ring it creates; apps record "PhoneMic Microphone". VB-CABLE is
//             only a fallback (Windows 10, or before the driver is installed).
//   Linux   — PulseAudio/PipeWire's own modules: a null sink "PhoneMic" plus a
//             remapped source "PhoneMic Microphone", created with pactl at
//             startup and removed on exit. Nothing to install.
//   macOS   — PhoneMic.driver (pc-app/driver-macos), a Core Audio plug-in
//             loopback device "PhoneMic Microphone": we play into it, apps
//             record it. BlackHole is only a fallback.
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
    // Ready through a third-party driver (VB-CABLE / BlackHole) rather than ours.
    bool third_party = false;
    std::string via;             // "PhoneMic driver", "PulseAudio", "VB-CABLE", ...
    std::string playback_match;  // output device to route the mic audio into
    std::string mic_name;        // what the user picks in Discord / Zoom / OBS
    std::string help;            // what to do when not ready
    // PhoneMic's own driver package found next to the app ("" = none), for the
    // Install button.
    std::string driver_package;
};

// Detect (and on Linux, create) the virtual microphone. Call once at startup,
// before opening audio devices.
VirtualMic setup_virtual_mic(EventLog* log);
// Undo what setup created (Linux: unload the pactl modules we loaded).
void teardown_virtual_mic();

// Install PhoneMic's own driver from `vm.driver_package` (Windows: runs
// install-driver.ps1 elevated, in a console that explains each step; macOS:
// asks for an administrator password and runs install-driver.sh). Runs on a
// background thread; `done` gets the outcome. Re-run setup_virtual_mic after.
void install_driver_async(const VirtualMic& vm, std::function<void(bool ok, std::string message)> done);

#ifdef _WIN32
// Fallback: download VB-CABLE, verify the installer's Authenticode signature,
// and launch it elevated. Runs on a background thread.
void install_vbcable_async(std::function<void(bool ok, std::string message)> done);
#endif

}  // namespace phonemic
