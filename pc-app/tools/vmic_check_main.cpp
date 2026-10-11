// phonemic-vmic-check: proves a virtual microphone really carries audio.
//
// Plays a 997 Hz tone into a playback device and records a capture device;
// passes when the tone comes back: loud enough, and the dominant component of
// what was recorded.
//
//   phonemic-vmic-check --setup          use the app's own virtual-mic setup
//                                        (Linux: creates the PulseAudio/PipeWire
//                                        devices; macOS: finds PhoneMic.driver)
//   phonemic-vmic-check [--play NAME] [--capture NAME] [--seconds 3] [--list]
//
// Names match exactly, else as a substring. CI runs it on Linux (PulseAudio)
// and on macOS after installing PhoneMic.driver.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "miniaudio.h"
#include "phonemic/audio_out.hpp"  // icontains
#include "phonemic/util.hpp"
#include "phonemic/virtual_mic.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr ma_uint32 kRate = 48000;
constexpr double kToneHz = 997.0;

struct Capture {
    std::vector<float> buf;
    std::atomic<size_t> len{0};
};

struct Tone {
    double phase = 0;
};

void play_cb(ma_device* d, void* out, const void*, ma_uint32 frames) {
    auto* t = static_cast<Tone*>(d->pUserData);
    auto* o = static_cast<float*>(out);
    for (ma_uint32 i = 0; i < frames; ++i) {
        o[i] = static_cast<float>(0.25 * std::sin(t->phase));  // -12 dBFS
        t->phase += 2 * kPi * kToneHz / kRate;
        if (t->phase > 2 * kPi) t->phase -= 2 * kPi;
    }
}

void capture_cb(ma_device* d, void*, const void* in, ma_uint32 frames) {
    auto* c = static_cast<Capture*>(d->pUserData);
    const size_t at = c->len.load(std::memory_order_relaxed);
    const size_t n = std::min<size_t>(frames, c->buf.size() - at);
    if (n) std::memcpy(c->buf.data() + at, in, n * sizeof(float));
    c->len.store(at + n, std::memory_order_release);
}

// Power of one frequency (Goertzel), normalised like a DFT bin.
double goertzel(const float* x, size_t n, double hz) {
    const double w = 2 * kPi * hz / kRate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (static_cast<double>(n) * n / 4.0);
}

bool find(ma_device_info* list, ma_uint32 n, const std::string& name, ma_device_id* id, std::string* found) {
    for (int pass = 0; pass < 2; ++pass)
        for (ma_uint32 i = 0; i < n; ++i)
            if (pass == 0 ? name == list[i].name : phonemic::icontains(list[i].name, name)) {
                *id = list[i].id;
                *found = list[i].name;
                return true;
            }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string play_match = "PhoneMic", cap_match = "PhoneMic";
    double seconds = 3;
    bool list_only = false, setup = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--play" && i + 1 < argc) play_match = argv[++i];
        else if (a == "--capture" && i + 1 < argc) cap_match = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::max(1.0, std::atof(argv[++i]));
        else if (a == "--list") list_only = true;
        else if (a == "--setup") setup = true;
        else {
            std::fprintf(stderr,
                         "usage: phonemic-vmic-check [--setup] [--play NAME] [--capture NAME] [--seconds N] [--list]\n");
            return 2;
        }
    }
    struct Teardown {
        bool on = false;
        ~Teardown() {
            if (on) phonemic::teardown_virtual_mic();
        }
    } teardown;
    if (setup) {
        phonemic::EventLog log{20, true};
        const phonemic::VirtualMic vm = phonemic::setup_virtual_mic(&log);
        teardown.on = true;
        if (!vm.ready) {
            std::fprintf(stderr, "FAIL: virtual microphone not ready: %s\n", vm.help.c_str());
            return 1;
        }
        play_match = vm.playback_match;
        cap_match = vm.mic_name;
        std::printf("app setup: play into \"%s\", apps record \"%s\"\n", play_match.c_str(), cap_match.c_str());
    }

    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
        std::fprintf(stderr, "FAIL: no audio backend\n");
        return 1;
    }
    ma_device_info *play_list = nullptr, *cap_list = nullptr;
    ma_uint32 play_n = 0, cap_n = 0;
    ma_context_get_devices(&ctx, &play_list, &play_n, &cap_list, &cap_n);
    std::printf("playback devices:\n");
    for (ma_uint32 i = 0; i < play_n; ++i) std::printf("  %s\n", play_list[i].name);
    std::printf("capture devices:\n");
    for (ma_uint32 i = 0; i < cap_n; ++i) std::printf("  %s\n", cap_list[i].name);
    if (list_only) {
        ma_context_uninit(&ctx);
        return 0;
    }

    ma_device_id play_id{}, cap_id{};
    std::string play_name, cap_name;
    if (!find(play_list, play_n, play_match, &play_id, &play_name) ||
        !find(cap_list, cap_n, cap_match, &cap_id, &cap_name)) {
        std::fprintf(stderr, "FAIL: no playback device \"%s\" + capture device \"%s\"\n", play_match.c_str(),
                     cap_match.c_str());
        ma_context_uninit(&ctx);
        return 1;
    }
    std::printf("playing into \"%s\", recording \"%s\"\n", play_name.c_str(), cap_name.c_str());

    Tone tone;
    Capture cap;
    cap.buf.assign(static_cast<size_t>(seconds * kRate) + kRate, 0.0f);

    ma_device_config pc = ma_device_config_init(ma_device_type_playback);
    pc.playback.pDeviceID = &play_id;
    pc.playback.format = ma_format_f32;
    pc.playback.channels = 1;
    pc.sampleRate = kRate;
    pc.dataCallback = play_cb;
    pc.pUserData = &tone;
    ma_device_config cc = ma_device_config_init(ma_device_type_capture);
    cc.capture.pDeviceID = &cap_id;
    cc.capture.format = ma_format_f32;
    cc.capture.channels = 1;
    cc.sampleRate = kRate;
    cc.dataCallback = capture_cb;
    cc.pUserData = &cap;

    ma_device play, rec;
    if (ma_device_init(&ctx, &pc, &play) != MA_SUCCESS) {
        std::fprintf(stderr, "FAIL: could not open the playback side\n");
        return 1;
    }
    if (ma_device_init(&ctx, &cc, &rec) != MA_SUCCESS) {
        std::fprintf(stderr, "FAIL: could not open the capture side\n");
        ma_device_uninit(&play);
        return 1;
    }
    ma_device_start(&play);
    ma_device_start(&rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
    ma_device_stop(&rec);
    ma_device_stop(&play);
    ma_device_uninit(&rec);
    ma_device_uninit(&play);
    ma_context_uninit(&ctx);

    const size_t got = cap.len.load(std::memory_order_acquire);
    // Judge the second half (the first includes device start-up).
    if (got < kRate / 2) {
        std::fprintf(stderr, "FAIL: only %zu samples recorded\n", got);
        return 1;
    }
    const float* x = cap.buf.data() + got / 2;
    const size_t n = got - got / 2;
    double energy = 0;
    for (size_t i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
    const double rms = std::sqrt(energy / static_cast<double>(n));
    const double rms_db = 20 * std::log10(std::max(rms, 1e-9));
    // Fraction of the signal's power that is the tone (a sine of amplitude A has
    // mean power A^2/2; the Goertzel bin above measures A^2).
    const double tone_share = goertzel(x, n, kToneHz) / 2.0 / std::max(energy / static_cast<double>(n), 1e-12);
    std::printf("recorded %zu samples: level %.1f dBFS, tone share %.0f%%\n", got, rms_db, tone_share * 100);

    if (rms_db < -30) {
        std::fprintf(stderr, "FAIL: recorded silence (expected about -15 dBFS)\n");
        return 1;
    }
    if (tone_share < 0.8) {
        std::fprintf(stderr, "FAIL: the recording is not the tone that was played\n");
        return 1;
    }
    std::printf("PASS: audio played into \"%s\" comes out of \"%s\"\n", play_name.c_str(), cap_name.c_str());
    return 0;
}
