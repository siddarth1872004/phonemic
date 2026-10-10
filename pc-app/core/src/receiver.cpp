#include "phonemic/receiver.hpp"

#include <phonemic/crypto.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phonemic {

namespace {
constexpr int kRecvTimeoutMs = 50;
constexpr uint32_t kStatusIntervalMs = 500;
constexpr size_t kMaxCachedKeys = 8;
constexpr size_t kMaxKdfPerSecond = 3;
constexpr size_t kMaxHelloPerSecond = 10;

const char* state_text(uint8_t s) {
    switch (s) {
        case PM_STATUS_OK: return "ok";
        case PM_STATUS_PIN_REQUIRED: return "rejected unencrypted audio (a PIN is set on this PC)";
        case PM_STATUS_PIN_MISSING: return "encrypted audio arrived but no PIN is set on this PC";
        case PM_STATUS_PIN_WRONG: return "PIN mismatch (could not decrypt)";
        case PM_STATUS_BUSY: return "ignored: another device is already streaming";
        case PM_STATUS_UNSUPPORTED: return "unsupported audio format";
    }
    return "?";
}
}  // namespace

std::string SourceId::describe() const {
    if (web) return "browser " + ep.ip_string();
    return ep.ip_string();
}

Receiver::Receiver(EventLog* log) : log_(log) {
    resampler_.set_rates(PM_SAMPLE_RATE, PM_SAMPLE_RATE);
    scratch_in_.reserve(PM_MAX_PAYLOAD);
    scratch_rs_.reserve(PM_MAX_PAYLOAD * 4);
    scratch_dn_.reserve(PM_MAX_PAYLOAD * 4);
    plain_.resize(PM_MAX_PAYLOAD);
    host_name_ = net::host_name();
}

Receiver::~Receiver() {
    stop();
    for (auto& [s, k] : keys_) crypto_wipe(k.key, sizeof k.key);
}

bool Receiver::start(uint16_t port, std::string* err) {
    stop();
    if (!sock_.open(port, err)) return false;
    port_ = sock_.local_port();
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { thread_main(); });
    if (log_) log_->add("Listening on UDP port " + std::to_string(port_));
    return true;
}

void Receiver::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    sock_.close();
    running_.store(false);
}

void Receiver::set_mic_sink(AudioSinkIface* s) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    mic_ = s;
}

void Receiver::set_monitor_sink(AudioSinkIface* s) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    monitor_sink_ = s;
}

void Receiver::set_passphrase(const std::string& p) {
    std::lock_guard<std::mutex> lk(settings_mu_);
    passphrase_ = p;
}

void Receiver::set_host_name(const std::string& n) {
    std::lock_guard<std::mutex> lk(settings_mu_);
    host_name_ = n;
}

void Receiver::set_packet_observer(PacketObserver f) {
    std::lock_guard<std::mutex> lk(settings_mu_);
    observer_ = std::move(f);
}

void Receiver::thread_main() {
    std::vector<uint8_t> buf(PM_MAX_DATAGRAM + 64);
    while (!stop_.load()) {
        net::Endpoint from;
        const int n = sock_.recv(buf.data(), buf.size(), &from, kRecvTimeoutMs);
        if (n > 0) handle_datagram(buf.data(), static_cast<size_t>(n), from);
        std::lock_guard<std::mutex> lk(pipe_mu_);
        periodic(mono_ms());
    }
}

void Receiver::handle_datagram(const uint8_t* data, size_t len, const net::Endpoint& from) {
    const uint64_t now = mono_ms();
    datagrams_.fetch_add(1, std::memory_order_relaxed);
    last_datagram_ms_.store(now, std::memory_order_relaxed);

    uint8_t version = 0, kind = 0;
    if (pm_peek(data, len, &version, &kind) != PM_OK) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (kind == PM_KIND_AUDIO) {
        SourceId src;
        src.ep = from;
        std::lock_guard<std::mutex> lk(pipe_mu_);
        process_audio(src, data, len, /*trusted=*/false, now);
    } else if (kind == PM_KIND_HELLO) {
        std::lock_guard<std::mutex> lk(pipe_mu_);
        answer_hello(data, len, from, now);
    }
    // ANNOUNCE / STATUS are PC->phone only; anything else is ignored.
}

uint8_t Receiver::submit(const SourceId& src, const uint8_t* data, size_t len, bool trusted) {
    const uint64_t now = mono_ms();
    datagrams_.fetch_add(1, std::memory_order_relaxed);
    last_datagram_ms_.store(now, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(pipe_mu_);
    return process_audio(src, data, len, trusted, now);
}

void Receiver::release_web(uint64_t web_conn) {
    std::lock_guard<std::mutex> lk(pipe_mu_);
    if (have_active_ && active_.web && active_.web_conn == web_conn) {
        have_active_ = false;
        if (log_) log_->add("Browser mic disconnected (" + active_.ep.ip_string() + ")");
    }
}

bool Receiver::key_for(uint32_t session, const std::string& pass, uint8_t out[32], uint64_t now) {
    if (pass != keys_pass_) {
        for (auto& [s, k] : keys_) crypto_wipe(k.key, sizeof k.key);
        keys_.clear();
        keys_pass_ = pass;
    }
    auto it = keys_.find(session);
    if (it != keys_.end()) {
        it->second.used_ms = now;
        std::memcpy(out, it->second.key, 32);
        return true;
    }
    // Argon2id is deliberately expensive; cap derivations so a flood of fake
    // session ids can't pin a CPU core.
    kdf_times_.erase(std::remove_if(kdf_times_.begin(), kdf_times_.end(),
                                    [&](uint64_t t) { return now - t >= 1000; }),
                     kdf_times_.end());
    if (kdf_times_.size() >= kMaxKdfPerSecond) return false;
    kdf_times_.push_back(now);

    KeyEntry e{};
    if (pm_derive_key(pass.data(), pass.size(), session, e.key) != 0) return false;
    e.used_ms = now;
    if (keys_.size() >= kMaxCachedKeys) {
        auto oldest = std::min_element(keys_.begin(), keys_.end(), [](const auto& a, const auto& b) {
            return a.second.used_ms < b.second.used_ms;
        });
        crypto_wipe(oldest->second.key, 32);
        keys_.erase(oldest);
    }
    std::memcpy(out, e.key, 32);
    keys_[session] = e;
    return true;
}

void Receiver::reject(const SourceId& src, uint8_t state, uint64_t now) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    const bool changed = last_reject_.load() != state || now - last_reject_ms_.load() > 10000;
    last_reject_.store(state);
    last_reject_ms_.store(now);
    last_reject_peer_ = src.describe();
    if (changed && log_) log_->add(src.describe() + ": " + state_text(state));
    if (!src.web && now - last_reject_reply_ms_ >= 250) {
        last_reject_reply_ms_ = now;
        send_status(src.ep, state, src.session, now);
    }
}

void Receiver::begin_session(const SourceId& src, const pm_audio_header& h, uint64_t now) {
    have_active_ = true;
    active_ = src;
    active_last_ms_ = now;
    active_rate_ = h.sample_rate;
    active_encrypted_ = (h.flags & PM_FLAG_ENCRYPTED) != 0;
    active_version_ = h.version;
    jitter_.reset();
    resampler_.set_rates(h.sample_rate, PM_SAMPLE_RATE);
    resampler_.reset();
    concealer_.reset();
    denoiser_.reset();
    jit_have_ = false;
    jitter_us_ = 0;
    consecutive_late_ = 0;
    session_received_.store(0);
    session_lost_.store(0);
    last_status_ms_ = 0;
    if (mic_) mic_->flush();
    if (monitor_sink_) monitor_sink_->flush();
    if (log_) {
        std::string m = (src.web ? "Browser mic connected: " : "Phone connected: ") + src.ep.ip_string();
        m += active_encrypted_ ? " (encrypted" : (src.web ? " (TLS" : " (unencrypted");
        m += ", " + std::to_string(h.sample_rate) + " Hz)";
        if (h.version == PM_VERSION_LEGACY) m += " — old phone app, please update";
        log_->add(m);
    }
}

uint8_t Receiver::process_audio(const SourceId& src_in, const uint8_t* data, size_t len,
                                bool trusted, uint64_t now) {
    pm_audio_header h{};
    const uint8_t* payload = nullptr;
    if (pm_decode_audio(data, len, &h, &payload) != PM_OK) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return PM_STATUS_UNSUPPORTED;
    }
    SourceId src = src_in;
    src.session = h.session_id;

    const bool is_active = have_active_ && active_.same_sender(src);
    if (!is_active && have_active_ && now - active_last_ms_ < kSessionTimeoutMs) {
        reject(src, PM_STATUS_BUSY, now);
        return PM_STATUS_BUSY;
    }
    if (h.codec != PM_CODEC_PCM16) {
        reject(src, PM_STATUS_UNSUPPORTED, now);
        return PM_STATUS_UNSUPPORTED;
    }

    std::string pass;
    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        pass = passphrase_;
    }
    const uint8_t* pcm = payload;
    size_t pcm_len = h.payload_len;
    if (h.flags & PM_FLAG_ENCRYPTED) {
        if (h.version == PM_VERSION_LEGACY) {
            // v1 used a weak key derivation that is no longer supported.
            legacy_encrypted_.store(true);
            reject(src, PM_STATUS_UNSUPPORTED, now);
            return PM_STATUS_UNSUPPORTED;
        }
        if (pass.empty()) {
            reject(src, PM_STATUS_PIN_MISSING, now);
            return PM_STATUS_PIN_MISSING;
        }
        uint8_t key[32];
        if (!key_for(h.session_id, pass, key, now)) return PM_STATUS_OK;  // rate-limited: drop quietly
        const int ok = pm_open(key, data, h.header_len, h.session_id, h.seq, h.timestamp_us,
                               payload, h.payload_len, plain_.data());
        crypto_wipe(key, sizeof key);
        if (ok != 0) {
            reject(src, PM_STATUS_PIN_WRONG, now);
            return PM_STATUS_PIN_WRONG;
        }
        pcm = plain_.data();
        pcm_len = h.payload_len - PM_TAG_LEN;
    } else if (!pass.empty() && !trusted) {
        // A PIN on this PC means "only accept authenticated audio" — otherwise
        // anyone on the network could inject sound into your microphone.
        reject(src, PM_STATUS_PIN_REQUIRED, now);
        return PM_STATUS_PIN_REQUIRED;
    }
    if (pcm_len == 0 || pcm_len % 2 != 0) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return PM_STATUS_UNSUPPORTED;
    }

    if (!is_active) begin_session(src, h, now);
    active_last_ms_ = now;
    if (h.sample_rate != active_rate_) {
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

    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        if (observer_) observer_(h, wall_us());
    }

    if (jitter_.insert(h.seq, std::vector<uint8_t>(pcm, pcm + pcm_len)) ==
        JitterBuffer::Insert::TooLate) {
        // A legacy (session-less) sender that restarted reuses low sequence
        // numbers: after a run of "late" packets, start over from this one.
        if (++consecutive_late_ >= 5) {
            consecutive_late_ = 0;
            jitter_.reset();
            jitter_.insert(h.seq, std::vector<uint8_t>(pcm, pcm + pcm_len));
        }
    } else {
        consecutive_late_ = 0;
    }

    for (;;) {
        auto p = jitter_.pop_ready(kReorderDepth);
        if (p.kind == JitterBuffer::PopKind::Empty) break;
        if (p.kind == JitterBuffer::PopKind::Conceal) {
            lost_.fetch_add(1, std::memory_order_relaxed);
            session_lost_.fetch_add(1, std::memory_order_relaxed);
            conceal_frame();
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

    if (!src.web && now - last_status_ms_ >= kStatusIntervalMs) send_status(src.ep, PM_STATUS_OK, src.session, now);
    return PM_STATUS_OK;
}

void Receiver::conceal_frame() {
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

void Receiver::send_status(const net::Endpoint& to, uint8_t state, uint32_t session, uint64_t now) {
    if (!sock_.is_open()) return;
    pm_status s{};
    s.state = state;
    s.session_id = session;
    s.buffer_ms = static_cast<uint16_t>(std::min<uint32_t>(mic_ ? mic_->buffered_ms() : 0, 65535));
    s.received = session_received_.load();
    s.lost = session_lost_.load();
    uint8_t out[PM_STATUS_LEN];
    pm_encode_status(out, &s);
    sock_.send_to(to, out, sizeof out);
    if (state == PM_STATUS_OK) last_status_ms_ = now;
}

void Receiver::answer_hello(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now) {
    uint32_t nonce = 0;
    if (pm_decode_hello(data, len, &nonce) != PM_OK) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    hello_times_.erase(std::remove_if(hello_times_.begin(), hello_times_.end(),
                                      [&](uint64_t t) { return now - t >= 1000; }),
                       hello_times_.end());
    if (hello_times_.size() >= kMaxHelloPerSecond) return;
    hello_times_.push_back(now);

    pm_announce a{};
    a.nonce = nonce;
    a.audio_port = port_;
    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        if (!passphrase_.empty()) a.flags |= PM_ANNOUNCE_FLAG_PIN;
        std::strncpy(a.name, host_name_.c_str(), PM_MAX_NAME);
        a.name[PM_MAX_NAME] = '\0';
    }
    if (have_active_ && now - active_last_ms_ < kSessionTimeoutMs) a.flags |= PM_ANNOUNCE_FLAG_BUSY;
    uint8_t out[PM_ANNOUNCE_MIN_LEN + PM_MAX_NAME];
    const int n = pm_encode_announce(out, sizeof out, &a);
    if (n > 0) sock_.send_to(from, out, static_cast<size_t>(n));
}

void Receiver::periodic(uint64_t now) {
    if (!have_active_) return;
    if (now - active_last_ms_ >= kSessionTimeoutMs) {
        have_active_ = false;
        peak_.store(0.0f);
        if (log_) log_->add((active_.web ? "Browser mic stopped: " : "Phone stopped streaming: ") + active_.ep.ip_string());
        return;
    }
    if (!active_.web && now - last_status_ms_ >= kStatusIntervalMs) send_status(active_.ep, PM_STATUS_OK, active_.session, now);
}

Receiver::Snapshot Receiver::snapshot() const {
    Snapshot s;
    const uint64_t now = mono_ms();
    s.running = running_.load();
    s.port = port_;
    s.datagrams = datagrams_.load();
    s.packets = packets_.load();
    s.lost = lost_.load();
    s.malformed = malformed_.load();
    s.rejected = rejected_.load();
    s.last_reject = last_reject_.load();
    s.last_reject_ms = last_reject_ms_.load();
    s.last_datagram_ms = last_datagram_ms_.load();
    s.last_packet_ms = last_packet_ms_.load();
    s.legacy_encrypted_seen = legacy_encrypted_.load();
    s.peak = peak_.load();
    s.voice_prob = vad_.load();
    std::lock_guard<std::mutex> lk(pipe_mu_);
    s.last_reject_peer = last_reject_peer_;
    s.connected = have_active_ && now - active_last_ms_ < 1500;
    if (have_active_) {
        s.peer = active_.describe();
        s.peer_is_web = active_.web;
        s.encrypted = active_encrypted_;
        s.sender_version = active_version_;
        s.sample_rate = active_rate_;
        s.jitter_ms = static_cast<float>(jitter_us_ / 1000.0);
    }
    s.mic_buffer_ms = mic_ ? mic_->buffered_ms() : 0;
    return s;
}

}  // namespace phonemic
