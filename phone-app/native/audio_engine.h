// PhoneMic native audio engine (Phase 0).
//
// Owns the Oboe input stream and a UDP socket, and streams PCM16 packets to the
// PC on a realtime audio thread. This is the audio hot path — no JNI calls, no
// allocation, and no locks happen inside onAudioReady().
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <oboe/Oboe.h>

namespace phonemic {

class AudioEngine : public oboe::AudioStreamDataCallback {
public:
    AudioEngine() = default;
    ~AudioEngine() override;

    // Open the mic and start streaming PCM16 to pc_ip:pc_port. If `pin` is
    // non-empty, payloads are encrypted (XChaCha20-Poly1305, key from the PIN).
    // Returns false if the socket or audio stream could not be started.
    bool start(const std::string& pc_ip, uint16_t pc_port, const std::string& pin);

    // Stop streaming and release the stream + socket. Idempotent.
    void stop();

    int64_t packets_sent() const { return packets_sent_.load(std::memory_order_relaxed); }
    float input_level() const { return input_level_.load(std::memory_order_relaxed); }
    // Audio session id of the input stream, so Kotlin can attach Android audio
    // effects (NoiseSuppressor / AEC / AGC). -1 until the stream is started.
    int32_t session_id() const { return session_id_.load(std::memory_order_relaxed); }

    // oboe::AudioStreamDataCallback
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* stream, void* audio_data, int32_t num_frames) override;

private:
    std::shared_ptr<oboe::AudioStream> stream_;
    int socket_fd_ = -1;
    uint32_t seq_ = 0;

    std::atomic<int64_t> packets_sent_{0};
    std::atomic<float> input_level_{0.0f};
    std::atomic<int32_t> session_id_{-1};

    bool encrypt_ = false;
    uint8_t key_[32] = {0};

    // Scratch send buffer: header + up to a callback's worth of PCM16. Sized for
    // the worst realistic callback; larger frames are split across sends.
    static constexpr int kMaxPayloadBytes = 4096;
};

}  // namespace phonemic
