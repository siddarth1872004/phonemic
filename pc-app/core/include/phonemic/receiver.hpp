// The receive pipeline: packets from the phone in -> clean audio out.
//
//   packet ─► decode (shared wire.h) ─► sender lock / PIN ─► decrypt ─► jitter
//          ─► buffer (reorder,
//          ─► loss detection) ─► Opus decode / PCM16 → float ─► resample to
//          ─► 48 kHz ─► loss concealment ─► Voice Focus (RNNoise) ─► voice
//          ─► changer ─► gain + limiter ─► mute ─► mic sink (+ monitor)
//
// Transport-agnostic: Bluetooth (bluetooth.hpp), USB (usb.hpp) and Wi-Fi
// (wifi.hpp) feed packets through submit() and send the STATUS feedback
// make_status() builds back to the phone.
//
// A PIN set on the PC means Wi-Fi audio must be sealed with it (crypto.h):
// otherwise anyone on the network could talk into your microphone. Bluetooth
// (paired, radio-encrypted) and USB (the cable, through adb) are trusted.
//
// One sender at a time: the first phone owns the mic until it disconnects or has
// been silent for kSessionTimeoutMs; anyone else gets STATUS busy.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <phonemic/wire.h>

#include "phonemic/denoise.hpp"
#include "phonemic/dsp.hpp"
#include "phonemic/jitter_buffer.hpp"
#include "phonemic/opus_codec.hpp"
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

// Identifies a sender: one connection (a Bluetooth or USB stream, or a Wi-Fi
// address) and the stream session on it.
struct SourceId {
    enum class Via : uint8_t { Bluetooth, Usb, Wifi };
    Via via = Via::Bluetooth;
    uint64_t conn_id = 0;      // transport connection id (Receiver::new_conn_id)
    uint32_t session = 0;      // from the packet
    std::string name;          // the phone's Bluetooth name, or its Wi-Fi address
    bool same_sender(const SourceId& o) const { return conn_id == o.conn_id && session == o.session; }
    std::string describe() const;
};

const char* via_name(SourceId::Via v);  // "Bluetooth", "USB", "Wi-Fi"

class Receiver {
public:
    static constexpr uint32_t kSessionTimeoutMs = 2000;
    static constexpr uint32_t kReorderDepth = 2;

    explicit Receiver(EventLog* log = nullptr);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Outputs. Pointers must stay valid until replaced or the receiver stops.
    void set_mic_sink(AudioSinkIface* s);
    void set_monitor_sink(AudioSinkIface* s);

    // Settings (thread-safe, take effect immediately).
    void set_voice_focus(bool on) { voice_focus_.store(on); }
    void set_effect(Effect e) { effect_.store(static_cast<uint8_t>(e)); }
    void set_gain_db(float db) { gain_db_.store(db); }
    void set_mute(bool m) { mute_.store(m); }
    void set_monitor(bool m) { monitor_.store(m); }
    // Wi-Fi PIN / passphrase; empty = Wi-Fi audio may be plaintext.
    void set_passphrase(const std::string& p);
    bool has_passphrase() const;

    // A connection id no other transport uses.
    static uint64_t new_conn_id();
    // True while a sender owns the mic (for discovery's "busy" flag).
    bool busy() const;

    // Feed one packet (thread-safe). Returns a PM_STATUS_* code describing what
    // happened, for the transport to report back to the phone.
    uint8_t submit(const SourceId& src, const uint8_t* data, size_t len);
    // The STATUS report for `src` (counters are 0 unless it is the active sender).
    pm_status make_status(const SourceId& src, uint8_t state) const;
    // A connection closed: release the mic if it owned it.
    void release_conn(uint64_t conn_id);

    struct Snapshot {
        bool connected = false;      // packets from the active sender within 1.5 s
        std::string peer;            // active sender
        SourceId::Via via = SourceId::Via::Bluetooth;
        bool encrypted = false;      // the active sender seals its audio with the PIN
        uint32_t session_received = 0, session_lost = 0;
        uint32_t sample_rate = 0;
        bool opus = false;           // the sender compresses with Opus
        uint64_t fec_recovered = 0;  // lost frames rebuilt from Opus FEC
        uint64_t datagrams = 0, packets = 0, lost = 0, malformed = 0;
        uint64_t rejected = 0;
        uint8_t last_reject = PM_STATUS_OK;
        std::string last_reject_peer;
        uint64_t last_reject_ms = 0, last_datagram_ms = 0, last_packet_ms = 0;
        float peak = 0;              // post-processing level, 0..1
        float voice_prob = 0;        // RNNoise VAD
        float jitter_ms = 0;         // RFC 3550 interarrival jitter
        uint32_t mic_buffer_ms = 0;
    };
    Snapshot snapshot() const;

private:
    uint8_t process_audio(const SourceId& src, const uint8_t* data, size_t len, uint64_t now);
    void reject(const SourceId& src, uint8_t state, uint64_t now);
    void begin_session(const SourceId& src, const pm_audio_header& h, uint64_t now);
    void play_frame(const float* in48k, size_t n);
    void conceal_frame();
    bool key_for(uint32_t session, const std::string& pass, uint8_t out[32], uint64_t now);

    EventLog* log_;

    // Settings.
    std::atomic<bool> voice_focus_{true}, mute_{false}, monitor_{false};
    std::atomic<uint8_t> effect_{0};
    std::atomic<float> gain_db_{0.0f};
    mutable std::mutex settings_mu_;
    std::string passphrase_;

    // Pipeline state — guarded by pipe_mu_ (one thread per connection submits).
    mutable std::mutex pipe_mu_;
    AudioSinkIface* mic_ = nullptr;
    AudioSinkIface* monitor_sink_ = nullptr;
    bool have_active_ = false;
    SourceId active_;
    uint64_t active_last_ms_ = 0;
    uint32_t active_rate_ = PM_SAMPLE_RATE;
    uint8_t active_codec_ = PM_CODEC_PCM16;
    bool active_encrypted_ = false;
    struct KeyEntry {
        uint8_t key[32];
        uint64_t used_ms;
    };
    std::map<uint32_t, KeyEntry> keys_;  // session id -> derived key
    std::string keys_pass_;              // passphrase the cache was built for
    std::vector<uint64_t> kdf_times_;    // rate limit for key derivations
    std::vector<uint8_t> plain_;
    OpusVoiceDecoder opus_;
    JitterBuffer jitter_{64, 0};
    Resampler resampler_;
    Concealer concealer_;
    Denoiser denoiser_;
    VoiceFx fx_;
    GainLimiter gain_;
    std::vector<float> scratch_in_, scratch_rs_, scratch_dn_;
    size_t last_frame_len_ = 480;
    // Jitter estimate (RFC 3550).
    bool jit_have_ = false;
    int64_t jit_prev_transit_ = 0;
    double jitter_us_ = 0;

    // Stats (atomics so snapshot() is cheap).
    std::atomic<uint64_t> datagrams_{0}, packets_{0}, lost_{0}, malformed_{0}, rejected_{0};
    std::atomic<uint64_t> last_datagram_ms_{0}, last_packet_ms_{0}, last_reject_ms_{0};
    std::atomic<uint8_t> last_reject_{PM_STATUS_OK};
    std::atomic<float> peak_{0.0f}, vad_{0.0f};
    std::atomic<uint32_t> session_received_{0}, session_lost_{0}, session_fec_{0};
    std::atomic<uint64_t> fec_recovered_{0};
    std::string last_reject_peer_;  // under pipe_mu_
};

}  // namespace phonemic
