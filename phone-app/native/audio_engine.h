// PhoneMic native audio engine.
//
// Oboe captures the mic on a realtime thread; onAudioReady() only copies the
// samples into a lock-free ring and wakes the sender thread (no syscalls, no
// locks, no allocation on the audio thread). The sender thread cuts fixed 10 ms
// frames, frames them with the shared wire.h, optionally encrypts them, sends
// them over UDP, and reads the PC's STATUS feedback.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <semaphore.h>

#include <oboe/Oboe.h>

struct OpusEncoder;

namespace phonemic {

class AudioEngine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    // start() results (mirrored in NativeBridge.kt).
    enum Result { kOk = 0, kBadAddress = 1, kSocketError = 2, kAudioError = 3, kCryptoError = 4 };

    AudioEngine();
    ~AudioEngine() override;

    // Open the mic and stream to pc_ip:pc_port. A non-empty `pin` encrypts
    // every packet. `voice_focus` picks the platform's voice-processing input
    // (noise suppression / echo cancellation) vs. a minimally processed one.
    // `opus` compresses each 10 ms frame (~48 kbps instead of 768 kbps PCM).
    int start(const std::string& pc_ip, uint16_t pc_port, const std::string& pin, bool voice_focus,
              bool opus);
    void stop();  // idempotent
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }

    // ---- telemetry for the UI (all lock-free) ----
    bool running() const { return running_.load(); }
    int64_t packets_sent() const { return packets_sent_.load(std::memory_order_relaxed); }
    float input_level() const { return input_level_.load(std::memory_order_relaxed); }
    int32_t audio_session_id() const { return audio_session_.load(std::memory_order_relaxed); }
    int32_t sample_rate() const { return sample_rate_.load(std::memory_order_relaxed); }
    int32_t stream_generation() const { return stream_gen_.load(std::memory_order_relaxed); }
    bool using_opus() const { return opus_active_.load(std::memory_order_relaxed); }
    // Latest STATUS from the PC: state (PM_STATUS_*, -1 = none yet) and age.
    int32_t pc_state() const { return pc_state_.load(std::memory_order_relaxed); }
    int64_t pc_age_ms() const;
    int32_t pc_buffer_ms() const { return pc_buffer_ms_.load(std::memory_order_relaxed); }
    int64_t pc_received() const { return pc_received_.load(std::memory_order_relaxed); }
    int64_t pc_lost() const { return pc_lost_.load(std::memory_order_relaxed); }
    int32_t last_send_errno() const { return last_errno_.load(std::memory_order_relaxed); }

    // oboe callbacks
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audio_data,
                                          int32_t num_frames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    bool open_stream();
    void close_stream();
    void sender_loop();
    void send_frame(const int16_t* pcm, size_t n);
    void poll_status();

    // ---- capture -> sender ring (single producer / single consumer) ----
    static constexpr size_t kRingSize = 1 << 15;  // samples (~680 ms @ 48 kHz)
    std::vector<int16_t> ring_;
    std::atomic<uint64_t> ring_write_{0}, ring_read_{0};
    std::atomic<int64_t> ring_overruns_{0};
    sem_t wake_;

    std::shared_ptr<oboe::AudioStream> stream_;
    std::thread sender_;
    std::atomic<bool> running_{false}, need_reopen_{false}, muted_{false};
    bool voice_focus_ = true;
    bool want_opus_ = false;
    OpusEncoder* opus_ = nullptr;          // used only on the sender thread
    std::atomic<bool> opus_active_{false};
    std::vector<uint8_t> encoded_;

    int socket_fd_ = -1;
    sockaddr_in dest_{};
    bool encrypt_ = false;
    uint8_t key_[32] = {0};
    uint32_t session_ = 0;
    uint32_t seq_ = 0;
    uint64_t start_us_ = 0, samples_sent_ = 0;
    std::vector<uint8_t> packet_;
    std::vector<int16_t> frame_;

    std::atomic<int64_t> packets_sent_{0};
    std::atomic<float> input_level_{0.0f};
    std::atomic<int32_t> audio_session_{-1}, sample_rate_{48000}, stream_gen_{0};
    std::atomic<int32_t> pc_state_{-1}, pc_buffer_ms_{0}, last_errno_{0};
    std::atomic<int64_t> pc_last_ms_{0}, pc_received_{0}, pc_lost_{0};
};

}  // namespace phonemic
