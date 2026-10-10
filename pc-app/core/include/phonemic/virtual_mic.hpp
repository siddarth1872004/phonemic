// Turning the received audio into a microphone other apps can select.
//
//   Windows — VB-CABLE: we play into "CABLE Input"; apps record "CABLE Output".
//             (One-click download + signature-checked install from the app.)
//   Linux   — PulseAudio/PipeWire: a null sink "PhoneMic" plus a remapped source
//             "PhoneMic Microphone", created with pactl at startup, removed on exit.
//   macOS   — BlackHole (if installed): we play into it; apps record from it.
#pragma once

#include <functional>
#include <string>

#include "phonemic/util.hpp"

namespace phonemic {

struct VirtualMic {
    bool ready = false;
    std::string playback_match;  // output device to route the mic audio into
    std::string mic_name;        // what the user picks in Discord / Zoom / OBS
    std::string help;            // what to do when not ready
};

// Detect (and on Linux, create) the virtual microphone. Call once at startup,
// before opening audio devices.
VirtualMic setup_virtual_mic(EventLog* log);
// Undo what setup created (Linux: unload the pactl modules we loaded).
void teardown_virtual_mic();

#ifdef _WIN32
// Download VB-CABLE, verify the installer's Authenticode signature, and launch
// it elevated. Runs on a background thread; `done` is called with the outcome.
void install_vbcable_async(std::function<void(bool ok, std::string message)> done);
#endif

}  // namespace phonemic
