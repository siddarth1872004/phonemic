#include "phonemic/audio_out.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>

#include "miniaudio.h"

namespace phonemic {

bool icontains(const std::string& h, const std::string& n) {
    if (n.empty()) return true;
    auto it = std::search(h.begin(), h.end(), n.begin(), n.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != h.end();
}

// ---- AudioSystem ----------------------------------------------------------------

struct AudioSystem::Impl {
    ma_context ctx{};
    bool ok = false;
    std::mutex mu;
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {
    ma_context_config cfg = ma_context_config_init();
#if defined(__linux__)
    // Prefer PulseAudio (also PipeWire's pulse server) so the "PhoneMic" virtual
    // sink is addressable by name; fall back to ALSA.
    ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa, ma_backend_jack, ma_backend_null};
    impl_->ok = ma_context_init(backends, 4, &cfg, &impl_->ctx) == MA_SUCCESS;
#else
    impl_->ok = ma_context_init(nullptr, 0, &cfg, &impl_->ctx) == MA_SUCCESS;
#endif
}

AudioSystem::~AudioSystem() {
    if (impl_->ok) ma_context_uninit(&impl_->ctx);
}

AudioSystem& AudioSystem::get() {
    static AudioSystem sys;
    return sys;
}

bool AudioSystem::ok() const { return impl_->ok; }
void* AudioSystem::context() { return impl_->ok ? &impl_->ctx : nullptr; }

std::string AudioSystem::backend_name() const {
    return impl_->ok ? ma_get_backend_name(impl_->ctx.backend) : "none";
}

std::vector<AudioDeviceInfo> AudioSystem::playback_devices() {
    std::vector<AudioDeviceInfo> out;
    if (!impl_->ok) return out;
    std::lock_guard<std::mutex> lk(impl_->mu);
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return out;
    for (ma_uint32 i = 0; i < count; ++i) out.push_back({infos[i].name, infos[i].isDefault != 0});
    return out;
}

// ---- AudioOutput ------------------------------------------------------------------

struct AudioOutput::Impl {
    ma_device dev{};
    std::atomic<bool> open{false};  // read by the receiver thread in write()
    std::string name;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()), playout_(48000, 1000) {}

AudioOutput::~AudioOutput() { close(); }

void AudioOutput::data_callback(void* device, void* out, const void*, unsigned frames) {
    auto* self = static_cast<AudioOutput*>(static_cast<ma_device*>(device)->pUserData);
    self->playout_.render(static_cast<float*>(out), frames);
}

namespace {
// Correctly-typed trampoline for miniaudio (realtime thread: no locks, no allocation).
void ma_data_trampoline(ma_device* d, void* out, const void* in, ma_uint32 frames);
}  // namespace

bool AudioOutput::open(const std::string& match, std::string* err) {
    close();
    auto& sys = AudioSystem::get();
    if (!sys.ok()) {
        if (err) *err = "no audio backend available";
        return false;
    }
    auto* ctx = static_cast<ma_context*>(sys.context());

    ma_device_id chosen{};
    bool have_id = false;
    matched_ = false;
    if (!match.empty()) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (int pass = 0; pass < 2 && !have_id; ++pass) {
                for (ma_uint32 i = 0; i < count; ++i) {
                    const std::string n = infos[i].name;
                    if (pass == 0 ? n == match : icontains(n, match)) {
                        chosen = infos[i].id;
                        impl_->name = n;
                        have_id = true;
                        break;
                    }
                }
            }
        }
        matched_ = have_id;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = have_id ? &chosen : nullptr;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;  // mono, duplicated to all device channels
    cfg.sampleRate = 48000;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = ma_data_trampoline;
    cfg.pUserData = this;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;  // render() always fills every sample

    if (ma_device_init(ctx, &cfg, &impl_->dev) != MA_SUCCESS) {
        if (err) *err = "could not open audio device" + (have_id ? " \"" + impl_->name + "\"" : std::string());
        return false;
    }
    if (!have_id) {
        char name[256] = {0};
        size_t len = 0;
        if (ma_device_get_name(&impl_->dev, ma_device_type_playback, name, sizeof name, &len) == MA_SUCCESS)
            impl_->name = name;
        else
            impl_->name = "default output";
    }
    if (ma_device_start(&impl_->dev) != MA_SUCCESS) {
        ma_device_uninit(&impl_->dev);
        if (err) *err = "could not start audio device \"" + impl_->name + "\"";
        return false;
    }
    impl_->open = true;
    return true;
}

void AudioOutput::close() {
    if (!impl_->open) return;
    ma_device_uninit(&impl_->dev);
    impl_->open = false;
    playout_.request_flush();
}

bool AudioOutput::is_open() const { return impl_->open; }
std::string AudioOutput::device_name() const { return impl_->name; }

void AudioOutput::write(const float* s, size_t n) {
    if (impl_->open) playout_.write(s, n);
}

uint32_t AudioOutput::buffered_ms() const {
    return impl_->open ? static_cast<uint32_t>(playout_.stats().fill_ms) : 0;
}

namespace {
void ma_data_trampoline(ma_device* d, void* out, const void* in, ma_uint32 frames) {
    AudioOutput::data_callback(d, out, in, frames);
}
}  // namespace

}  // namespace phonemic
