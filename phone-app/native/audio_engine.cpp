#include "audio_engine.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "phonemic", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "phonemic", __VA_ARGS__)

namespace phonemic {

namespace {

// --- Wire protocol (mirror of the /protocol Rust crate) ----------------------
// TODO(phase1): replace this with the cbindgen-generated header so there is a
// single source of truth. Kept minimal and in lockstep with docs/PROTOCOL.md.
constexpr uint16_t kMagic = 0x4D50;  // "PM", little-endian
constexpr uint8_t kVersion = 1;
constexpr uint8_t kCodecPcm16 = 0;
constexpr int kHeaderLen = 18;

// Capture format. 48 kHz mono matches the PC receiver; 10 ms per Oboe callback
// is a reasonable low-latency target (AAudio may hand us less).
constexpr int32_t kSampleRate = 48000;
constexpr int32_t kChannels = 1;

void write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void write_u32_le(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
void write_u64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

uint64_t now_micros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

sockaddr_in g_dest{};  // set in start(), read in the callback

}  // namespace

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const std::string& pc_ip, uint16_t pc_port) {
    stop();  // ensure clean state

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
        ->setDataCallback(this);

    oboe::Result result = builder.openStream(stream_);
    if (result != oboe::Result::OK) {
        LOGE("openStream failed: %s", oboe::convertToText(result));
        stop();
        return false;
    }

    seq_ = 0;
    packets_sent_.store(0);
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
    // header math and the send buffer stay bounded.
    uint8_t buf[kHeaderLen + kMaxPayloadBytes];
    int32_t offset_bytes = 0;
    while (offset_bytes < payload_bytes) {
        int32_t chunk = payload_bytes - offset_bytes;
        if (chunk > kMaxPayloadBytes) chunk = kMaxPayloadBytes;

        write_u16_le(buf + 0, kMagic);
        buf[2] = kVersion;
        buf[3] = kCodecPcm16;
        write_u32_le(buf + 4, seq_++);
        write_u64_le(buf + 8, now_micros());
        write_u16_le(buf + 16, static_cast<uint16_t>(chunk));
        std::memcpy(buf + kHeaderLen,
                    reinterpret_cast<const uint8_t*>(samples) + offset_bytes, chunk);

        ::sendto(socket_fd_, buf, kHeaderLen + chunk, 0,
                 reinterpret_cast<sockaddr*>(&g_dest), sizeof(g_dest));
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        offset_bytes += chunk;
    }

    return oboe::DataCallbackResult::Continue;
}

}  // namespace phonemic
