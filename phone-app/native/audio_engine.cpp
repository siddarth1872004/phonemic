#include "audio_engine.h"

#include <android/log.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include <phonemic/wire.h>  // shared with the PC: the one packet format

#include <opus.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "phonemic", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "phonemic", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "phonemic", __VA_ARGS__)

namespace phonemic {

namespace {

constexpr int32_t kSampleRate = PM_SAMPLE_RATE;
// Frames waiting for the Bluetooth writer. More than this (~200 ms) is pure
// added latency, so the oldest are dropped instead.
constexpr size_t kMaxQueued = 20;

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
    packet_.resize(PM_FRAME_PREFIX_LEN + PM_MAX_DATAGRAM);
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

int AudioEngine::start(bool voice_focus) {
    stop();  // clean state

    // A fresh random session per start: the PC uses it to tell restarts apart.
    session_ = random_session();
    seq_ = 0;
    samples_sent_ = 0;
    reset_link();

    voice_focus_ = voice_focus;
    ring_write_.store(0);
    ring_read_.store(0);
    packets_sent_.store(0);
    pc_state_.store(-1);
    pc_last_ms_.store(0);
    pc_received_.store(0);
    pc_lost_.store(0);

    if (!open_stream()) return kAudioError;
    // Opus (~48 kbps, comfortably inside a Bluetooth link) runs at 48 kHz, which
    // Oboe resamples to; fall back to PCM otherwise.
    if (sample_rate_.load() == PM_SAMPLE_RATE) {
        int err = 0;
        opus_ = opus_encoder_create(PM_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (err == OPUS_OK && opus_) {
            opus_encoder_ctl(opus_, OPUS_SET_BITRATE(48000));
            opus_encoder_ctl(opus_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(opus_, OPUS_SET_COMPLEXITY(5));        // light on phone CPUs
            opus_encoder_ctl(opus_, OPUS_SET_INBAND_FEC(1));       // lets the PC rebuild lost frames
            opus_encoder_ctl(opus_, OPUS_SET_PACKET_LOSS_PERC(5));
            opus_encoder_ctl(opus_, OPUS_SET_VBR_CONSTRAINT(1));
            encoded_.resize(1500);
        } else {
            LOGW("opus encoder init failed (%d); sending PCM", err);
            opus_ = nullptr;
        }
    }
    opus_active_.store(opus_ != nullptr);
    start_us_ = now_us();
    running_.store(true);
    sender_ = std::thread([this] { sender_loop(); });
    LOGI("capturing @ %d Hz, session %08x, %s", sample_rate_.load(), session_, opus_ ? "opus" : "pcm");
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
    out_cv_.notify_all();  // wake the Bluetooth writer: take_outgoing() returns -1
    close_stream();
    if (opus_) {
        opus_encoder_destroy(opus_);
        opus_ = nullptr;
    }
    opus_active_.store(false);
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
    }
}

void AudioEngine::send_frame(const int16_t* pcm, size_t n) {
    pm_audio_header h{};
    // Opus only takes 48 kHz 10 ms frames. If the stream was reopened at another
    // rate mid-session, drop frames rather than switch codec (the PC would treat
    // a codec switch as a brand-new session).
    const bool opus = opus_ != nullptr && n == static_cast<size_t>(PM_SAMPLE_RATE / 100);
    if (opus_ != nullptr && !opus) return;  // rate changed under Opus: drop until it recovers
    h.codec = opus ? PM_CODEC_OPUS : PM_CODEC_PCM16;
    h.session_id = session_;
    h.seq = seq_++;
    // A media clock (like RTP): capture start + samples so far. The PC's jitter
    // estimate then measures the network, not this thread's scheduling.
    h.timestamp_us = start_us_ + samples_sent_ * 1000000ull / static_cast<uint64_t>(sample_rate_.load());
    h.sample_rate = static_cast<uint32_t>(sample_rate_.load());
    samples_sent_ += n;

    const uint8_t* body = reinterpret_cast<const uint8_t*>(pcm);
    size_t bytes = n * sizeof(int16_t);  // little-endian on all Android ABIs
    if (opus) {
        const int len = opus_encode(opus_, pcm, static_cast<int>(n), encoded_.data(),
                                    static_cast<opus_int32>(encoded_.size()));
        if (len <= 0) return;
        body = encoded_.data();
        bytes = static_cast<size_t>(len);
    }
    const int r = pm_encode_audio(packet_.data() + PM_FRAME_PREFIX_LEN, packet_.size() - PM_FRAME_PREFIX_LEN, &h,
                                  body, bytes);
    if (r < 0) return;
    pm_write_frame_prefix(packet_.data(), static_cast<size_t>(r));
    queue(packet_.data(), PM_FRAME_PREFIX_LEN + static_cast<size_t>(r));
}

void AudioEngine::queue(const uint8_t* packet, size_t len) {
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        // Not connected, or the link stalled: keep only the newest frames.
        while (out_.size() >= kMaxQueued) out_.pop_front();
        out_.emplace_back(packet, packet + len);
    }
    out_cv_.notify_one();
    packets_sent_.fetch_add(1, std::memory_order_relaxed);
}

int AudioEngine::take_outgoing(uint8_t* buf, size_t cap, int timeout_ms) {
    std::unique_lock<std::mutex> lk(out_mu_);
    out_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !out_.empty() || !running_.load(); });
    if (!running_.load()) return -1;
    size_t n = 0;
    while (!out_.empty() && n + out_.front().size() <= cap) {
        std::memcpy(buf + n, out_.front().data(), out_.front().size());
        n += out_.front().size();
        out_.pop_front();
    }
    return static_cast<int>(n);
}

void AudioEngine::reset_link() {
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        out_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(in_mu_);
        in_.clear();
    }
    pc_state_.store(-1);
    pc_last_ms_.store(0);
}

void AudioEngine::handle_status(const uint8_t* data, size_t len) {
    pm_status s{};
    if (pm_decode_status(data, len, &s) != PM_OK) return;
    if (s.session_id != session_) return;  // stale report from an older session
    pc_state_.store(s.state, std::memory_order_relaxed);
    pc_buffer_ms_.store(s.buffer_ms, std::memory_order_relaxed);
    pc_received_.store(s.received, std::memory_order_relaxed);
    pc_lost_.store(s.lost, std::memory_order_relaxed);
    pc_last_ms_.store(static_cast<int64_t>(now_us() / 1000), std::memory_order_relaxed);
}

void AudioEngine::feed_incoming(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(in_mu_);
    in_.insert(in_.end(), data, data + len);
    size_t pos = 0;
    while (in_.size() - pos >= PM_FRAME_PREFIX_LEN) {
        const size_t n = pm_r16(in_.data() + pos);
        if (n == 0 || n > 512) {  // not a PhoneMic STATUS stream: resync from scratch
            in_.clear();
            return;
        }
        if (in_.size() - pos < PM_FRAME_PREFIX_LEN + n) break;
        handle_status(in_.data() + pos + PM_FRAME_PREFIX_LEN, n);
        pos += PM_FRAME_PREFIX_LEN + n;
    }
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos));
}

}  // namespace phonemic
