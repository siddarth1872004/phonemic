// Turning the received audio into a microphone other apps can select.
// PhoneMic installs no driver of its own:
//
//   Windows — VB-CABLE, a free, signed virtual audio cable (vb-audio.com),
//             installed from the app. PhoneMic plays into its playback side
//             ("CABLE Input") and apps record its other side ("CABLE Output").
//             The app can rename the two endpoints "PhoneMic Feed" and
//             "PhoneMic Microphone", and keeps the cable from becoming the
//             default speaker (which would send every PC sound into the mic).
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
    std::string via;             // "VB-CABLE", "PipeWire", "PulseAudio", ...
    std::string playback_match;  // output device to route the mic audio into
    std::string mic_name;        // what the user picks in Discord / Zoom / OBS
    std::string help;            // what to do when not ready
    // Windows: VB-CABLE can be installed from the app (it isn't yet), and its
    // endpoints still carry their stock names (so they can be renamed).
    bool can_install = false;
    bool can_rename = false;
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

#ifdef _WIN32
// Download VB-CABLE from vb-audio.com, check the installer's Authenticode
// signature (valid, issued to VB-Audio), and launch it elevated. Runs on a
// background thread; `done` gets the outcome. Re-run setup_virtual_mic after.
void install_vbcable_async(std::function<void(bool ok, std::string message)> done);

struct CableEndpoints {
    std::string feed_name;        // playback side, e.g. "CABLE Input (VB-Audio Virtual Cable)"
    std::string mic_name;         // recording side, e.g. "CABLE Output (VB-Audio Virtual Cable)"
    bool named_phonemic = false;  // already renamed to PhoneMic Feed / PhoneMic Microphone
    bool default_output = false;  // the cable is the default speaker (bad: PC sound leaks into the mic)
};
bool find_cable(CableEndpoints* out);
// Move the default speaker off the cable onto a real device. true = changed.
bool fix_cable_default_output(std::string* now_default);
// Rename the cable's endpoints to PhoneMic's names (or back to VB-CABLE's).
// The async form relaunches this program elevated with `--rename-cable` /
// `--restore-cable`, whose handler calls rename_cable_now (0 = done).
void rename_cable_async(bool to_phonemic, std::function<void(bool ok, std::string message)> done);
int rename_cable_now(bool to_phonemic);
#endif

}  // namespace phonemic
