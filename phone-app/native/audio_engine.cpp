#include "audio_engine.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <random>

#include <phonemic/crypto.h>  // shared with the PC: XChaCha20-Poly1305 + Argon2id
#include <phonemic/wire.h>    // shared with the PC: the one packet format

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "phonemic", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "phonemic", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "phonemic", __VA_ARGS__)

namespace phonemic {

namespace {

constexpr int32_t kSampleRate = PM_SAMPLE_RATE;

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t random_session() {
    uint32_t v = 0;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::read(fd, &v, sizeof v) != static_cast<ssize_t>(sizeof v)) v = 0;
        ::close(fd);
    }
    if (v == 0) v = static_cast<uint32_t>(std::random_device{}()) | 1u;
    return v;
}

}  // namespace

AudioEngine::AudioEngine() : ring_(kRingSize, 0) {
    sem_init(&wake_, 0, 0);
    packet_.resize(PM_MAX_DATAGRAM);
}

AudioEngine::~AudioEngine() {
    stop();
    sem_destroy(&wake_);
}

int64_t AudioEngine::pc_age_ms() const {
    const int64_t last = pc_last_ms_.load(std::memory_order_relaxed);
    if (last == 0) return -1;
    return static_cast<int64_t>(now_us() / 1000) - last;
}

int AudioEngine::start(const std::string& pc_ip, uint16_t pc_port, const std::string& pin, bool voice_focus) {
    stop();  // clean state

    std::memset(&dest_, 0, sizeof dest_);
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(pc_port);
    if (::inet_pton(AF_INET, pc_ip.c_str(), &dest_.sin_addr) != 1) {
        LOGE("bad PC address: %s", pc_ip.c_str());
        return kBadAddress;
    }

    // A fresh random session per start: the PC uses it to tell restarts apart,
    // and (with a PIN) it salts the key so nonces never repeat across sessions.
    session_ = random_session();
    seq_ = 0;
    samples_sent_ = 0;
    encrypt_ = !pin.empty();
    if (encrypt_ && pm_derive_key(pin.data(), pin.size(), session_, key_) != 0) {
        LOGE("key derivation failed (out of memory)");
        return kCryptoError;
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return kSocketError;
    }
    // connect() fixes the peer: send() needs no address and recv() only accepts
    // datagrams from the PC (nobody else can spoof STATUS replies to us).
    if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&dest_), sizeof dest_) != 0) {
        LOGE("connect() failed: %s", strerror(errno));
        close_stream();
        ::close(socket_fd_);
        socket_fd_ = -1;
        return kSocketError;
    }

    voice_focus_ = voice_focus;
    ring_write_.store(0);
    ring_read_.store(0);
    packets_sent_.store(0);
    pc_state_.store(-1);
    pc_last_ms_.store(0);
    pc_received_.store(0);
    pc_lost_.store(0);
    last_errno_.store(0);

    if (!open_stream()) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return kAudioError;
    }
    start_us_ = now_us();
    running_.store(true);
    sender_ = std::thread([this] { sender_loop(); });
    LOGI("streaming to %s:%u @ %d Hz, session %08x%s", pc_ip.c_str(), pc_port, sample_rate_.load(), session_,
         encrypt_ ? ", encrypted" : "");
    return kOk;
}

bool AudioEngine::open_stream() {
    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)  // AAudio falls back to Shared if denied
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        // Devices whose mic doesn't run at 48 kHz get resampled here instead of
        // silently streaming the wrong rate (which pitch-shifted the old build).
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setFormatConversionAllowed(true)
        ->setChannelConversionAllowed(true)
        // Voice Focus on: the platform's voice pipeline (NS + AEC). Off: a
        // minimally processed source, so turning it off really turns it off.
        ->setInputPreset(voice_focus_ ? oboe::InputPreset::VoiceCommunication
                                      : oboe::InputPreset::VoiceRecognition)
        ->setSessionId(oboe::SessionId::Allocate)  // lets Kotlin attach NoiseSuppressor/AEC
        ->setDataCallback(this)
        ->setErrorCallback(this);

    oboe::Result r = b.openStream(stream_);
    if (r != oboe::Result::OK) {
        LOGE("openStream failed: %s", oboe::convertToText(r));
        stream_.reset();
        return false;
    }
    sample_rate_.store(stream_->getSampleRate());
    audio_session_.store(stream_->getSessionId());
    r = stream_->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(r));
        close_stream();
        return false;
    }
    stream_gen_.fetch_add(1);
    return true;
}

void AudioEngine::close_stream() {
    if (stream_) {
        stream_->stop();
        stream_->close();
        stream_.reset();
    }
}

void AudioEngine::stop() {
    if (running_.exchange(false)) {
        sem_post(&wake_);
        if (sender_.joinable()) sender_.join();
    }
    close_stream();
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    crypto_wipe(key_, sizeof key_);
    input_level_.store(0.0f);
    audio_session_.store(-1);
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream*, void* audio_data, int32_t num_frames) {
    const auto* in = static_cast<const int16_t*>(audio_data);
    const bool muted = muted_.load(std::memory_order_relaxed);

    // Peak for the meter (int arithmetic: |-32768| doesn't overflow).
    int32_t peak = 0;
    for (int32_t i = 0; i < num_frames; ++i) {
        const int32_t a = in[i] < 0 ? -static_cast<int32_t>(in[i]) : in[i];
        if (a > peak) peak = a;
    }
    input_level_.store(muted ? 0.0f : static_cast<float>(peak) / 32768.0f, std::memory_order_relaxed);

    // Copy into the SPSC ring; if the sender fell behind, drop (never block).
    const uint64_t w = ring_write_.load(std::memory_order_relaxed);
    const uint64_t r = ring_read_.load(std::memory_order_acquire);
    const uint64_t room = kRingSize - (w - r);
    const uint64_t n = static_cast<uint64_t>(num_frames) < room ? static_cast<uint64_t>(num_frames) : room;
    for (uint64_t i = 0; i < n; ++i) ring_[(w + i) & (kRingSize - 1)] = muted ? 0 : in[i];
    ring_write_.store(w + n, std::memory_order_release);
    if (n < static_cast<uint64_t>(num_frames)) ring_overruns_.fetch_add(1, std::memory_order_relaxed);

    sem_post(&wake_);  // async-signal-safe, never blocks
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    // E.g. a headset was plugged in or Bluetooth connected: the stream is gone.
    // Reopen it from the sender thread (not from this callback).
    LOGW("input stream closed: %s — reopening", oboe::convertToText(error));
    need_reopen_.store(true);
    sem_post(&wake_);
}

void AudioEngine::sender_loop() {
    while (running_.load()) {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        sem_timedwait(&wake_, &ts);
        if (!running_.load()) break;

        if (need_reopen_.exchange(false)) {
            stream_.reset();  // already closed by Oboe
            for (int attempt = 0; attempt < 10 && running_.load(); ++attempt) {
                if (open_stream()) break;
                usleep(200 * 1000);
            }
        }

        const size_t frame = static_cast<size_t>(sample_rate_.load() / 100);  // 10 ms
        if (frame_.size() != frame) frame_.assign(frame, 0);
        for (;;) {
            const uint64_t r = ring_read_.load(std::memory_order_relaxed);
            const uint64_t w = ring_write_.load(std::memory_order_acquire);
            if (w - r < frame) break;
            for (size_t i = 0; i < frame; ++i) frame_[i] = ring_[(r + i) & (kRingSize - 1)];
            ring_read_.store(r + frame, std::memory_order_release);
            send_frame(frame_.data(), frame);
        }
        poll_status();
    }
}

void AudioEngine::send_frame(const int16_t* pcm, size_t n) {
    pm_audio_header h{};
    h.codec = PM_CODEC_PCM16;
    h.session_id = session_;
    h.seq = seq_++;
    // A media clock (like RTP): capture start + samples so far. The PC's jitter
    // estimate then measures the network, not this thread's scheduling.
    h.timestamp_us = start_us_ + samples_sent_ * 1000000ull / static_cast<uint64_t>(sample_rate_.load());
    h.sample_rate = static_cast<uint32_t>(sample_rate_.load());
    samples_sent_ += n;

    const size_t bytes = n * sizeof(int16_t);  // little-endian on all Android ABIs
    size_t total;
    if (encrypt_) {
        h.flags = PM_FLAG_ENCRYPTED;
        h.payload_len = static_cast<uint16_t>(bytes + PM_TAG_LEN);
        pm_write_audio_header(packet_.data(), &h);  // header first: it is the AAD
        pm_seal(key_, packet_.data(), PM_AUDIO_HEADER_LEN, session_, h.seq, h.timestamp_us,
                reinterpret_cast<const uint8_t*>(pcm), bytes, packet_.data() + PM_AUDIO_HEADER_LEN);
        total = PM_AUDIO_HEADER_LEN + h.payload_len;
    } else {
        const int r = pm_encode_audio(packet_.data(), packet_.size(), &h, reinterpret_cast<const uint8_t*>(pcm), bytes);
        if (r < 0) return;
        total = static_cast<size_t>(r);
    }
    if (::send(socket_fd_, packet_.data(), total, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        // Wi-Fi dropped / no route: keep capturing, report it, retry next frame.
        last_errno_.store(errno, std::memory_order_relaxed);
    } else {
        last_errno_.store(0, std::memory_order_relaxed);
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::poll_status() {
    uint8_t buf[64];
    for (;;) {
        const ssize_t n = ::recv(socket_fd_, buf, sizeof buf, MSG_DONTWAIT);
        if (n <= 0) return;
        pm_status s{};
        if (pm_decode_status(buf, static_cast<size_t>(n), &s) != PM_OK) continue;
        if (s.session_id != session_) continue;  // stale report from an older session
        pc_state_.store(s.state, std::memory_order_relaxed);
        pc_buffer_ms_.store(s.buffer_ms, std::memory_order_relaxed);
        pc_received_.store(s.received, std::memory_order_relaxed);
        pc_lost_.store(s.lost, std::memory_order_relaxed);
        pc_last_ms_.store(static_cast<int64_t>(now_us() / 1000), std::memory_order_relaxed);
    }
}

}  // namespace phonemic
