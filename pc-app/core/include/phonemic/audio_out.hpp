// Audio output devices (miniaudio: WASAPI / PulseAudio / ALSA).
//
// The device is opened as 48 kHz mono float; miniaudio converts to whatever the
// hardware/virtual cable actually runs at (44.1 kHz, stereo, int16, ...). The
// old cpal-based sink forced 48 kHz and simply failed on 44.1 kHz devices.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "phonemic/playout.hpp"
#include "phonemic/receiver.hpp"

namespace phonemic {

struct AudioDeviceInfo {
    std::string name;
    bool is_default = false;
};

// Process-wide audio context (device enumeration + device creation).
class AudioSystem {
public:
    static AudioSystem& get();
    bool ok() const;
    std::string backend_name() const;
    std::vector<AudioDeviceInfo> playback_devices();
    void* context();  // ma_context*

private:
    AudioSystem();
    ~AudioSystem();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AudioOutput final : public AudioSinkIface {
public:
    AudioOutput();
    ~AudioOutput() override;
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // `match` empty = system default device; otherwise the first playback device
    // whose name equals `match`, else contains it (case-insensitive).
    // Returns false (with err) if no device could be opened.
    bool open(const std::string& match, std::string* err);
    void close();
    bool is_open() const;
    std::string device_name() const;
    bool matched() const { return matched_; }  // `match` was found (vs default fallback)

    void set_latency_ms(uint32_t ms) { playout_.set_target_ms(ms); }
    AdaptivePlayout::Stats stats() const { return playout_.stats(); }

    // AudioSinkIface
    void write(const float* s, size_t n) override;
    void flush() override { playout_.request_flush(); }
    uint32_t buffered_ms() const override;

    // Internal: invoked by the device's realtime thread.
    static void data_callback(void* device, void* out, const void* in, unsigned frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AdaptivePlayout playout_;
    bool matched_ = false;
};

// Case-insensitive substring search (exposed for tests).
bool icontains(const std::string& haystack, const std::string& needle);

}  // namespace phonemic
