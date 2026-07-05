#include "audio_engine.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "wire.h"    // shared framing, cross-checked against the Rust decoder
#include "crypto.h"  // XChaCha20-Poly1305 payload encryption (monocypher)

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "phonemic", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "phonemic", __VA_ARGS__)

namespace phonemic {

namespace {

// Capture format. 48 kHz mono matches the PC receiver; 10 ms per Oboe callback
// is a reasonable low-latency target (AAudio may hand us less).
constexpr int32_t kSampleRate = 48000;
constexpr int32_t kChannels = 1;

uint64_t now_micros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

sockaddr_in g_dest{};  // set in start(), read in the callback

}  // namespace

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const std::string& pc_ip, uint16_t pc_port,
                        const std::string& pin) {
    stop();  // ensure clean state

    // Derive the encryption key from the shared PIN (empty PIN = plaintext).
    encrypt_ = !pin.empty();
    if (encrypt_) {
        pm_derive_key(pin.c_str(), key_);
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        LOGE("socket() failed");
        return false;
    }
    std::memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(pc_port);
    if (::inet_pton(AF_INET, pc_ip.c_str(), &g_dest.sin_addr) != 1) {
        LOGE("bad PC IP: %s", pc_ip.c_str());
        stop();
        return false;
    }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(kChannels)
        ->setSampleRate(kSampleRate)
        ->setInputPreset(oboe::InputPreset::VoiceCommunication)
        // Allocate a session id so Kotlin can attach NoiseSuppressor/AEC/AGC.
        ->setSessionId(oboe::SessionId::Allocate)
        ->setDataCallback(this);

    oboe::Result result = builder.openStream(stream_);
    if (result != oboe::Result::OK) {
        LOGE("openStream failed: %s", oboe::convertToText(result));
        stop();
        return false;
    }

    seq_ = 0;
    packets_sent_.store(0);
    session_id_.store(stream_->getSessionId());
    result = stream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(result));
        stop();
        return false;
    }

    LOGI("streaming to %s:%u @ %d Hz", pc_ip.c_str(), pc_port, kSampleRate);
    return true;
}

void AudioEngine::stop() {
    if (stream_) {
        stream_->requestStop();
        stream_->close();
        stream_.reset();
    }
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
    oboe::AudioStream* /*stream*/, void* audio_data, int32_t num_frames) {
    const auto* samples = static_cast<const int16_t*>(audio_data);
    const int32_t sample_count = num_frames * kChannels;
    const int32_t payload_bytes = sample_count * static_cast<int32_t>(sizeof(int16_t));

    // Track a coarse peak level for the UI meter (no allocation, no lock).
    int16_t peak = 0;
    for (int32_t i = 0; i < sample_count; ++i) {
        int16_t a = samples[i] < 0 ? static_cast<int16_t>(-samples[i]) : samples[i];
        if (a > peak) peak = a;
    }
    input_level_.store(static_cast<float>(peak) / 32768.0f, std::memory_order_relaxed);

    // Chunk the callback into packets no bigger than kMaxPayloadBytes so the
    // header math and the send buffer stay bounded (+16 for the auth tag).
    uint8_t buf[PM_HEADER_LEN + kMaxPayloadBytes + PM_TAG_LEN];
    int32_t offset_bytes = 0;
    while (offset_bytes < payload_bytes) {
        int32_t chunk = payload_bytes - offset_bytes;
        if (chunk > kMaxPayloadBytes) chunk = kMaxPayloadBytes;

        const uint8_t* src = reinterpret_cast<const uint8_t*>(samples) + offset_bytes;
        uint32_t seq = seq_++;
        uint64_t ts = now_micros();
        int total;

        if (encrypt_) {
            // payload = ciphertext || 16-byte tag; header carries PM_ENCRYPTED
            // and the encrypted length. The header (AAD) is written first so it
            // authenticates the seq/timestamp the nonce is derived from.
            uint16_t enc_len = static_cast<uint16_t>(chunk + PM_TAG_LEN);
            pm_write_header(PM_CODEC_PCM16 | PM_ENCRYPTED, seq, ts, enc_len, buf);
            pm_encrypt(key_, buf, PM_HEADER_LEN, seq, ts, src,
                       static_cast<size_t>(chunk), buf + PM_HEADER_LEN);
            total = PM_HEADER_LEN + enc_len;
        } else {
            total = pm_encode(PM_CODEC_PCM16, 0, seq, ts, src,
                              static_cast<uint16_t>(chunk), buf);
        }

        ::sendto(socket_fd_, buf, total, 0,
                 reinterpret_cast<sockaddr*>(&g_dest), sizeof(g_dest));
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        offset_bytes += chunk;
    }

    return oboe::DataCallbackResult::Continue;
}

}  // namespace phonemic
