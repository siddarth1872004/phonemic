// The receive pipeline: UDP (and browser) packets in -> clean audio out.
//
//   datagram ─► decode (shared wire.h) ─► sender lock / auth policy ─► decrypt
//            ─► jitter buffer (reorder, loss detection) ─► PCM16 → float
//            ─► resample to 48 kHz ─► loss concealment ─► Voice Focus (RNNoise)
//            ─► voice changer ─► gain + limiter ─► mute ─► mic sink (+ monitor)
//
// Also answers discovery probes (HELLO -> ANNOUNCE) and sends STATUS feedback to
// the active phone twice a second, so the phone can show "connected", "wrong
// PIN", "PC busy" and the PC's buffer latency instead of streaming blind.
//
// One sender at a time: the first valid sender owns the mic until it has been
// silent for kSessionTimeoutMs; anyone else gets STATUS busy. (Previously two
// phones would interleave their packets into garbage.)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <phonemic/wire.h>

#include "phonemic/denoise.hpp"
#include "phonemic/dsp.hpp"
#include "phonemic/jitter_buffer.hpp"
#include "phonemic/net.hpp"
#include "phonemic/util.hpp"
#include "phonemic/voice_fx.hpp"

namespace phonemic {

// Where processed 48 kHz mono audio goes (an audio device, a test buffer, the
// driver ring...). write() is called from the receive thread.
class AudioSinkIface {
public:
    virtual ~AudioSinkIface() = default;
    virtual void write(const float* samples, size_t n) = 0;
    virtual void flush() {}                         // a new session started
    virtual uint32_t buffered_ms() const { return 0; }
};

// Identifies a sender: a UDP endpoint, or a browser connection.
struct SourceId {
    bool web = false;
    net::Endpoint ep;          // UDP sender (web: the browser's address)
    uint64_t web_conn = 0;     // web connection id
    uint32_t session = 0;      // from the packet
    bool same_sender(const SourceId& o) const {
        return web == o.web && ep == o.ep && web_conn == o.web_conn && session == o.session;
    }
    std::string describe() const;
};

class Receiver {
public:
    static constexpr uint32_t kSessionTimeoutMs = 2000;
    static constexpr uint32_t kReorderDepth = 2;

    explicit Receiver(EventLog* log = nullptr);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Bind UDP `port` and start the receive thread. Returns false + error text if
    // the port is busy (another PhoneMic running) etc.
    bool start(uint16_t port, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }

    // Outputs. Pointers must stay valid until replaced or the receiver stops.
    void set_mic_sink(AudioSinkIface* s);
    void set_monitor_sink(AudioSinkIface* s);

    // Settings (thread-safe, take effect immediately).
    void set_passphrase(const std::string& p);
    void set_voice_focus(bool on) { voice_focus_.store(on); }
    void set_effect(Effect e) { effect_.store(static_cast<uint8_t>(e)); }
    void set_gain_db(float db) { gain_db_.store(db); }
    void set_mute(bool m) { mute_.store(m); }
    void set_monitor(bool m) { monitor_.store(m); }
    void set_host_name(const std::string& n);

    // Feed one packet from a non-UDP transport (the browser WebSocket path).
    // `trusted` = the transport is already authenticated + encrypted (TLS +
    // pairing token), so plaintext is acceptable even when a PIN is set.
    // Returns a PM_STATUS_* code describing what happened.
    uint8_t submit(const SourceId& src, const uint8_t* data, size_t len, bool trusted);
    // A browser connection closed: release the mic if it owned it.
    void release_web(uint64_t web_conn);

    // Optional hook called for every accepted audio packet (latency measurement).
    using PacketObserver = std::function<void(const pm_audio_header&, uint64_t arrival_wall_us)>;
    void set_packet_observer(PacketObserver f);

    // Process a raw datagram as if it arrived on the socket (tests use this to
    // drive the pipeline without a network).
    void handle_datagram(const uint8_t* data, size_t len, const net::Endpoint& from);

    struct Snapshot {
        bool running = false;
        uint16_t port = 0;
        bool connected = false;      // packets from the active sender within 1.5 s
        std::string peer;            // active sender
        bool peer_is_web = false;
        bool encrypted = false;
        uint8_t sender_version = 0;  // 1 = legacy app
        uint32_t sample_rate = 0;
        uint64_t datagrams = 0, packets = 0, lost = 0, malformed = 0;
        uint64_t rejected = 0;
        uint8_t last_reject = PM_STATUS_OK;
        std::string last_reject_peer;
        uint64_t last_reject_ms = 0, last_datagram_ms = 0, last_packet_ms = 0;
        bool legacy_encrypted_seen = false;
        float peak = 0;              // post-processing level, 0..1
        float voice_prob = 0;        // RNNoise VAD
        float jitter_ms = 0;         // RFC 3550 interarrival jitter
        uint32_t mic_buffer_ms = 0;
    };
    Snapshot snapshot() const;

private:
    struct KeyEntry {
        uint8_t key[32];
        uint64_t used_ms;
    };

    void thread_main();
    uint8_t process_audio(const SourceId& src, const uint8_t* data, size_t len, bool trusted,
                          uint64_t now);
    void reject(const SourceId& src, uint8_t state, uint64_t now);
    bool key_for(uint32_t session, const std::string& pass, uint8_t out[32], uint64_t now);
    void begin_session(const SourceId& src, const pm_audio_header& h, uint64_t now);
    void play_frame(const float* in48k, size_t n);
    void conceal_frame();
    void send_status(const net::Endpoint& to, uint8_t state, uint32_t session, uint64_t now);
    void answer_hello(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now);
    void periodic(uint64_t now);

    EventLog* log_;
    net::UdpSocket sock_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    uint16_t port_ = 0;

    // Settings.
    std::atomic<bool> voice_focus_{true}, mute_{false}, monitor_{false};
    std::atomic<uint8_t> effect_{0};
    std::atomic<float> gain_db_{0.0f};
    mutable std::mutex settings_mu_;
    std::string passphrase_;
    std::string host_name_;
    PacketObserver observer_;

    // Pipeline state — guarded by pipe_mu_ (UDP thread and web threads).
    mutable std::mutex pipe_mu_;
    AudioSinkIface* mic_ = nullptr;
    AudioSinkIface* monitor_sink_ = nullptr;
    bool have_active_ = false;
    SourceId active_;
    uint64_t active_last_ms_ = 0;
    uint32_t active_rate_ = PM_SAMPLE_RATE;
    bool active_encrypted_ = false;
    uint8_t active_version_ = 0;
    JitterBuffer jitter_{64, 0};
    Resampler resampler_;
    Concealer concealer_;
    Denoiser denoiser_;
    VoiceFx fx_;
    GainLimiter gain_;
    std::vector<float> scratch_in_, scratch_rs_, scratch_dn_;
    std::vector<uint8_t> plain_;
    size_t last_frame_len_ = 480;
    uint32_t consecutive_late_ = 0;
    std::map<uint32_t, KeyEntry> keys_;  // session id -> derived key
    std::string keys_pass_;              // passphrase the cache was built for
    std::vector<uint64_t> kdf_times_;    // rate limit for key derivations
    uint64_t last_status_ms_ = 0;
    uint64_t last_reject_reply_ms_ = 0;
    std::vector<uint64_t> hello_times_;
    // Jitter estimate (RFC 3550).
    bool jit_have_ = false;
    int64_t jit_prev_transit_ = 0;
    double jitter_us_ = 0;

    // Stats (atomics so snapshot() is cheap).
    std::atomic<uint64_t> datagrams_{0}, packets_{0}, lost_{0}, malformed_{0}, rejected_{0};
    std::atomic<uint64_t> last_datagram_ms_{0}, last_packet_ms_{0}, last_reject_ms_{0};
    std::atomic<uint8_t> last_reject_{PM_STATUS_OK};
    std::atomic<bool> legacy_encrypted_{false};
    std::atomic<float> peak_{0.0f}, vad_{0.0f};
    std::atomic<uint32_t> session_received_{0}, session_lost_{0};
    std::string last_reject_peer_;  // under pipe_mu_
};

}  // namespace phonemic
