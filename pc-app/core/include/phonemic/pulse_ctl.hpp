// Linux: talk to the sound server (PulseAudio, or PipeWire through
// pipewire-pulse) to create PhoneMic's virtual microphone, without the pactl
// command-line tool (not installed by default on every distro).
//
// libpulse is loaded at run time (dlopen "libpulse.so.0"), like miniaudio does
// for audio, so the binary has no build- or link-time dependency on it and runs
// on systems without it (then there is simply no sound server to talk to).
#pragma once

#if defined(__linux__)

#include <cstdint>
#include <memory>
#include <string>

namespace phonemic {

class PulseCtl {
public:
    // nullptr if libpulse isn't installed or no sound server is running.
    static std::unique_ptr<PulseCtl> connect(std::string* err);
    ~PulseCtl();

    bool has_sink(const std::string& name);
    bool has_source(const std::string& name);
    // Module index, or nullopt-like UINT32_MAX on failure.
    uint32_t load_module(const std::string& name, const std::string& args);
    bool unload_module(uint32_t index);
    std::string server_name();  // e.g. "PulseAudio (on PipeWire 1.0.5)"
    std::string default_source();  // name of the default recording device
    bool set_default_source(const std::string& name);

    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

private:
    PulseCtl() = default;
    void query_server(std::string* name, std::string* default_source);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonemic

#endif
