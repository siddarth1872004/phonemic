// PhoneMic native audio engine.
//
// Oboe captures the mic on a realtime thread; onAudioReady() only copies the
// samples into a lock-free ring and wakes the sender thread (no syscalls, no
// locks, no allocation on the audio thread). The sender thread cuts fixed 10 ms
// frames, Opus-encodes them (and seals them with the PIN, for Wi-Fi), frames
// them with the shared wire.h ([u16 len][packet]) and queues them. Kotlin owns
// the link (Bluetooth RFCOMM is Java-only on Android; Wi-Fi UDP and USB TCP sit
// beside it): it drains that queue (take_outgoing) onto the socket and feeds
// the PC's STATUS replies back (feed_incoming).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <semaphore.h>

#include <oboe/Oboe.h>

struct OpusEncoder;

namespace phonemic {

class AudioEngine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    // start() results (mirrored in NativeBridge.kt).
    enum Result { kOk = 0, kAudioError = 3, kCryptoError = 4 };

    AudioEngine();
    ~AudioEngine() override;

    // Open the mic and start producing frames. `voice_focus` picks the
    // platform's voice-processing input (noise suppression / echo cancellation)
    // vs. a minimally processed one. A non-empty `pin` seals every packet with
    // it (Wi-Fi to a PC that has a PIN set); Bluetooth and USB pass "".
    int start(bool voice_focus, const std::string& pin);
    void stop();  // idempotent
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }

    // ---- the link (called from Kotlin's socket threads) ----
    // Copy whole queued frames into buf (waits up to timeout_ms for the first).
    // Returns bytes copied, 0 on timeout, -1 once stopped.
    int take_outgoing(uint8_t* buf, size_t cap, int timeout_ms);
    // Bytes the PC sent (framed STATUS packets), in any chunking.
    void feed_incoming(const uint8_t* data, size_t len);
    // A new connection: drop the stale backlog and any half-received frame.
    void reset_link();

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

    // oboe callbacks
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audio_data,
                                          int32_t num_frames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    bool open_stream();
    void close_stream();
    void sender_loop();
    void send_frame(const int16_t* pcm, size_t n);
    void handle_status(const uint8_t* data, size_t len);
    void queue(const uint8_t* packet, size_t len);

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
    OpusEncoder* opus_ = nullptr;          // used only on the sender thread
    std::atomic<bool> opus_active_{false};
    std::vector<uint8_t> encoded_;

    // ---- sender -> link writer (framed packets) ----
    std::mutex out_mu_;
    std::condition_variable out_cv_;
    std::deque<std::vector<uint8_t>> out_;
    std::mutex in_mu_;
    std::vector<uint8_t> in_;  // STATUS bytes not yet a whole frame

    uint32_t session_ = 0;
    bool encrypt_ = false;
    uint8_t key_[32] = {};  // PM_KEY_LEN, derived once per session
    uint32_t seq_ = 0;
    uint64_t start_us_ = 0, samples_sent_ = 0;
    std::vector<uint8_t> packet_;
    std::vector<int16_t> frame_;

    std::atomic<int64_t> packets_sent_{0};
    std::atomic<float> input_level_{0.0f};
    std::atomic<int32_t> audio_session_{-1}, sample_rate_{48000}, stream_gen_{0};
    std::atomic<int32_t> pc_state_{-1}, pc_buffer_ms_{0};
    std::atomic<int64_t> pc_last_ms_{0}, pc_received_{0}, pc_lost_{0};
};

}  // namespace phonemic
