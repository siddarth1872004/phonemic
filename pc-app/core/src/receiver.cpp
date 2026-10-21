#include "phonemic/receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phonemic {

namespace {
const char* state_text(uint8_t s) {
    switch (s) {
        case PM_STATUS_OK: return "ok";
        case PM_STATUS_BUSY: return "ignored: another phone is already streaming";
        case PM_STATUS_UNSUPPORTED: return "unsupported audio format";
    }
    return "?";
}
}  // namespace

Receiver::Receiver(EventLog* log) : log_(log) {
    resampler_.set_rates(PM_SAMPLE_RATE, PM_SAMPLE_RATE);
    scratch_in_.reserve(PM_MAX_PAYLOAD);
    scratch_rs_.reserve(PM_MAX_PAYLOAD * 4);
    scratch_dn_.reserve(PM_MAX_PAYLOAD * 4);
}

Receiver::~Receiver() = default;

void Receiver::set_mic_sink(AudioSinkIface* s) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    mic_ = s;
}

void Receiver::set_monitor_sink(AudioSinkIface* s) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    monitor_sink_ = s;
}

uint8_t Receiver::submit(const SourceId& src, const uint8_t* data, size_t len) {
    const uint64_t now = mono_ms();
    datagrams_.fetch_add(1, std::memory_order_relaxed);
    last_datagram_ms_.store(now, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(pipe_mu_);
    return process_audio(src, data, len, now);
}

pm_status Receiver::make_status(const SourceId& src, uint8_t state) const {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    pm_status s{};
    s.state = state;
    s.session_id = src.session;
    s.buffer_ms = static_cast<uint16_t>(std::min<uint32_t>(mic_ ? mic_->buffered_ms() : 0, 65535));
    if (have_active_ && active_.same_sender(src)) {
        s.received = session_received_.load();
        s.lost = session_lost_.load();
    }
    return s;
}

void Receiver::release_conn(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    if (have_active_ && active_.conn_id == conn_id) {
        have_active_ = false;
        peak_.store(0.0f);
        if (log_) log_->add(active_.describe() + " disconnected");
    }
}

void Receiver::reject(const SourceId& src, uint8_t state, uint64_t now) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    const bool changed = last_reject_.load() != state || now - last_reject_ms_.load() > 10000;
    last_reject_.store(state);
    last_reject_ms_.store(now);
    last_reject_peer_ = src.describe();
    if (changed && log_) log_->add(src.describe() + ": " + state_text(state));
}

void Receiver::begin_session(const SourceId& src, const pm_audio_header& h, uint64_t now) {
    have_active_ = true;
    active_ = src;
    active_last_ms_ = now;
    active_codec_ = h.codec;
    // Opus always decodes at 48 kHz, whatever rate the sender captured at.
    active_rate_ = h.codec == PM_CODEC_OPUS ? PM_SAMPLE_RATE : h.sample_rate;
    jitter_.reset();
    resampler_.set_rates(active_rate_, PM_SAMPLE_RATE);
    resampler_.reset();
    concealer_.reset();
    opus_.reset();
    session_fec_.store(0);
    denoiser_.reset();
    jit_have_ = false;
    jitter_us_ = 0;
    session_received_.store(0);
    session_lost_.store(0);
    if (mic_) mic_->flush();
    if (monitor_sink_) monitor_sink_->flush();
    if (log_)
        log_->add(src.describe() + " streaming (" +
                  (h.codec == PM_CODEC_OPUS ? std::string("Opus") : std::to_string(h.sample_rate) + " Hz PCM") + ")");
}

uint8_t Receiver::process_audio(const SourceId& src_in, const uint8_t* data, size_t len, uint64_t now) {
    pm_audio_header h{};
    const uint8_t* payload = nullptr;
    if (pm_decode_audio(data, len, &h, &payload) != PM_OK) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return PM_STATUS_UNSUPPORTED;
    }
    SourceId src = src_in;
    src.session = h.session_id;

    const bool is_active = have_active_ && active_.same_sender(src);
    // A new session on the owning connection is the same phone restarting its
    // stream; another connection waits until the owner disconnects or goes quiet.
    if (!is_active && have_active_ && active_.conn_id != src.conn_id && now - active_last_ms_ < kSessionTimeoutMs) {
        reject(src, PM_STATUS_BUSY, now);
        return PM_STATUS_BUSY;
    }
    if (h.codec != PM_CODEC_PCM16 && h.codec != PM_CODEC_OPUS) {
        reject(src, PM_STATUS_UNSUPPORTED, now);
        return PM_STATUS_UNSUPPORTED;
    }

    const uint8_t* pcm = payload;
    const size_t pcm_len = h.payload_len;
    if (pcm_len == 0 || (h.codec == PM_CODEC_PCM16 && pcm_len % 2 != 0)) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return PM_STATUS_UNSUPPORTED;
    }

    // A codec switch mid-stream can only mean the sender restarted: start over.
    if (!is_active || h.codec != active_codec_) begin_session(src, h, now);
    active_last_ms_ = now;
    if (h.codec == PM_CODEC_PCM16 && h.sample_rate != active_rate_) {
        active_rate_ = h.sample_rate;
        resampler_.set_rates(h.sample_rate, PM_SAMPLE_RATE);
    }
    packets_.fetch_add(1, std::memory_order_relaxed);
    last_packet_ms_.store(now, std::memory_order_relaxed);
    session_received_.fetch_add(1, std::memory_order_relaxed);

    // RFC 3550 interarrival jitter, in microseconds.
    const int64_t transit = static_cast<int64_t>(mono_us()) - static_cast<int64_t>(h.timestamp_us);
    if (jit_have_) {
        const double d = std::fabs(static_cast<double>(transit - jit_prev_transit_));
        // A huge jump means the sender's clock restarted; don't let it poison the average.
        if (d < 1e6) jitter_us_ += (d - jitter_us_) / 16.0;
    }
    jit_have_ = true;
    jit_prev_transit_ = transit;

    jitter_.insert(h.seq, std::vector<uint8_t>(pcm, pcm + pcm_len));  // too-late packets are dropped

    for (;;) {
        auto p = jitter_.pop_ready(kReorderDepth);
        if (p.kind == JitterBuffer::PopKind::Empty) break;
        if (p.kind == JitterBuffer::PopKind::Conceal) {
            lost_.fetch_add(1, std::memory_order_relaxed);
            session_lost_.fetch_add(1, std::memory_order_relaxed);
            conceal_frame();
            continue;
        }
        if (active_codec_ == PM_CODEC_OPUS) {
            if (!opus_.decode(p.payload.data(), p.payload.size(), scratch_rs_) || scratch_rs_.empty()) {
                malformed_.fetch_add(1, std::memory_order_relaxed);
                conceal_frame();  // a corrupt packet is just another lost frame
                continue;
            }
            last_frame_len_ = scratch_rs_.size();
            play_frame(scratch_rs_.data(), scratch_rs_.size());
            continue;
        }
        const size_t n = p.payload.size() / 2;
        scratch_in_.resize(n);
        pcm16le_to_float(p.payload.data(), n, scratch_in_.data());
        scratch_rs_.clear();
        resampler_.process(scratch_in_.data(), n, scratch_rs_);
        if (scratch_rs_.empty()) continue;
        concealer_.on_frame(scratch_rs_.data(), scratch_rs_.size());
        last_frame_len_ = scratch_rs_.size();
        play_frame(scratch_rs_.data(), scratch_rs_.size());
    }
    return PM_STATUS_OK;
}

void Receiver::conceal_frame() {
    if (active_codec_ == PM_CODEC_OPUS) {
        // Rebuild the lost frame from the next packet's in-band FEC when it has
        // arrived (it has whenever loss was declared after a reorder wait).
        const auto* next = jitter_.peek_next();
        if (opus_.conceal(next ? next->data() : nullptr, next ? next->size() : 0, last_frame_len_, scratch_rs_)) {
            fec_recovered_.fetch_add(1, std::memory_order_relaxed);
            session_fec_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!scratch_rs_.empty()) play_frame(scratch_rs_.data(), scratch_rs_.size());
        return;
    }
    scratch_rs_.resize(last_frame_len_);
    concealer_.conceal(scratch_rs_.data(), scratch_rs_.size());
    play_frame(scratch_rs_.data(), scratch_rs_.size());
}

void Receiver::play_frame(const float* in, size_t n) {
    // Copy so the chain can work in place (`in` may be scratch_rs_).
    scratch_in_.assign(in, in + n);
    float* buf = scratch_in_.data();
    size_t len = n;

    if (voice_focus_.load(std::memory_order_relaxed) && Denoiser::available()) {
        scratch_dn_.clear();
        denoiser_.process(buf, len, scratch_dn_);
        vad_.store(denoiser_.vad(), std::memory_order_relaxed);
        buf = scratch_dn_.data();
        len = scratch_dn_.size();
    }
    if (len == 0) return;

    fx_.set_effect(effect_from_u8(effect_.load(std::memory_order_relaxed)));
    fx_.process(buf, len);
    gain_.set_gain_db(gain_db_.load(std::memory_order_relaxed));
    gain_.process(buf, len);
    if (mute_.load(std::memory_order_relaxed)) std::fill(buf, buf + len, 0.0f);

    peak_.store(peak_abs(buf, len), std::memory_order_relaxed);
    if (mic_) mic_->write(buf, len);
    if (monitor_.load(std::memory_order_relaxed) && monitor_sink_ && monitor_sink_ != mic_)
        monitor_sink_->write(buf, len);
}

Receiver::Snapshot Receiver::snapshot() const {
    Snapshot s;
    const uint64_t now = mono_ms();
    s.datagrams = datagrams_.load();
    s.packets = packets_.load();
    s.lost = lost_.load();
    s.malformed = malformed_.load();
    s.rejected = rejected_.load();
    s.last_reject = last_reject_.load();
    s.last_reject_ms = last_reject_ms_.load();
    s.last_datagram_ms = last_datagram_ms_.load();
    s.last_packet_ms = last_packet_ms_.load();
    s.fec_recovered = fec_recovered_.load();
    std::lock_guard<std::mutex> lk(pipe_mu_);
    s.last_reject_peer = last_reject_peer_;
    s.connected = have_active_ && now - active_last_ms_ < 1500;
    s.peak = s.connected ? peak_.load() : 0.0f;
    s.voice_prob = s.connected ? vad_.load() : 0.0f;
    if (have_active_) {
        s.peer = active_.describe();
        s.session_received = session_received_.load();
        s.session_lost = session_lost_.load();
        s.sample_rate = active_rate_;
        s.opus = active_codec_ == PM_CODEC_OPUS;
        s.jitter_ms = static_cast<float>(jitter_us_ / 1000.0);
    }
    s.mic_buffer_ms = mic_ ? mic_->buffered_ms() : 0;
    return s;
}

}  // namespace phonemic
