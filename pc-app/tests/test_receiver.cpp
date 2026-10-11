// End-to-end tests of the receive pipeline: packets in -> processed audio out.
#include "phonemic/receiver.hpp"

#include <phonemic/crypto.h>
#include <phonemic/wire.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#include "test.hpp"

using namespace phonemic;

namespace {
struct CollectSink : AudioSinkIface {
    std::vector<float> got;
    int flushes = 0;
    void write(const float* s, size_t n) override { got.insert(got.end(), s, s + n); }
    void flush() override { ++flushes; }
};

std::vector<uint8_t> audio_packet(uint32_t session, uint32_t seq, const std::vector<int16_t>& pcm,
                                  uint32_t rate = 48000, const std::string& pass = "") {
    pm_audio_header h{};
    h.codec = PM_CODEC_PCM16;
    h.session_id = session;
    h.seq = seq;
    h.timestamp_us = 1000000ull + seq * 10000ull;
    h.sample_rate = rate;
    std::vector<uint8_t> payload(pcm.size() * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        payload[2 * i] = static_cast<uint8_t>(pcm[i] & 0xFF);
        payload[2 * i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(pcm[i]) >> 8) & 0xFF);
    }
    if (pass.empty()) {
        std::vector<uint8_t> out(PM_AUDIO_HEADER_LEN + payload.size());
        pm_encode_audio(out.data(), out.size(), &h, payload.data(), payload.size());
        return out;
    }
    h.flags = PM_FLAG_ENCRYPTED;
    h.payload_len = static_cast<uint16_t>(payload.size() + PM_TAG_LEN);
    std::vector<uint8_t> out(PM_AUDIO_HEADER_LEN + h.payload_len);
    pm_write_audio_header(out.data(), &h);
    uint8_t key[32];
    pm_derive_key(pass.data(), pass.size(), session, key);
    pm_seal(key, out.data(), PM_AUDIO_HEADER_LEN, session, seq, h.timestamp_us, payload.data(),
            payload.size(), out.data() + PM_AUDIO_HEADER_LEN);
    return out;
}

std::vector<int16_t> tone(int seq, size_t n = 480) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<int16_t>(8000 * std::sin(6.2831853 * 440.0 * (seq * static_cast<double>(n) + i) / 48000.0));
    return v;
}

net::Endpoint ep(const char* ip, uint16_t port) {
    net::Endpoint e;
    net::Endpoint::parse(ip, port, &e);
    return e;
}

struct Rig {
    Receiver rx;
    CollectSink mic, mon;
    Rig() {
        rx.set_voice_focus(false);  // keep the signal bit-predictable
        rx.set_mic_sink(&mic);
        rx.set_monitor_sink(&mon);
    }
    void feed(const std::vector<uint8_t>& p, const net::Endpoint& from = ep("192.168.1.20", 50000)) {
        rx.handle_datagram(p.data(), p.size(), from);
    }
};
}  // namespace

TEST(rx_plaintext_audio_reaches_the_mic_sink_unchanged) {
    Rig r;
    for (int s = 0; s < 10; ++s) r.feed(audio_packet(7, s, tone(s)));
    CHECK_EQ(r.mic.got.size(), 4800u);
    const auto t3 = tone(3);
    for (size_t i = 0; i < 480; ++i) CHECK_NEAR(r.mic.got[3 * 480 + i], t3[i] / 32768.0, 1e-6);
    const auto snap = r.rx.snapshot();
    CHECK(snap.connected);
    CHECK_EQ(snap.packets, 10u);
    CHECK_EQ(snap.lost, 0u);
    CHECK(!snap.encrypted);
    CHECK(r.mon.got.empty());  // monitor is off by default
}

TEST(rx_monitor_gets_a_copy_when_enabled) {
    Rig r;
    r.rx.set_monitor(true);
    for (int s = 0; s < 3; ++s) r.feed(audio_packet(7, s, tone(s)));
    CHECK_EQ(r.mon.got.size(), 1440u);
}

TEST(rx_reorders_and_conceals_losses) {
    Rig r;
    for (int s : {0, 1, 3, 2, 4, 6, 7, 8}) r.feed(audio_packet(9, s, tone(s)));  // 5 lost
    const auto snap = r.rx.snapshot();
    CHECK_EQ(snap.lost, 1u);
    CHECK_EQ(r.mic.got.size(), 9u * 480);  // 8 real frames + 1 concealed, in order
    const auto t2 = tone(2);
    CHECK_NEAR(r.mic.got[2 * 480 + 10], t2[10] / 32768.0, 1e-6);  // reordered 2 landed in slot 2
}

TEST(rx_pin_on_pc_rejects_plaintext_injection) {
    Rig r;
    r.rx.set_passphrase("hunter2");
    r.feed(audio_packet(1, 0, tone(0)));
    CHECK(r.mic.got.empty());
    const auto snap = r.rx.snapshot();
    CHECK_EQ(snap.last_reject, PM_STATUS_PIN_REQUIRED);
    CHECK(!snap.connected);
}

TEST(rx_decrypts_with_matching_pin_and_rejects_wrong_pin) {
    Rig r;
    r.rx.set_passphrase("hunter2");
    r.feed(audio_packet(5, 0, tone(0), 48000, "hunter2"));
    r.feed(audio_packet(5, 1, tone(1), 48000, "hunter2"));
    CHECK_EQ(r.mic.got.size(), 960u);
    CHECK(r.rx.snapshot().encrypted);

    Rig w;
    w.rx.set_passphrase("hunter3");
    w.feed(audio_packet(5, 0, tone(0), 48000, "hunter2"));
    CHECK(w.mic.got.empty());
    CHECK_EQ(w.rx.snapshot().last_reject, PM_STATUS_PIN_WRONG);
}

TEST(rx_encrypted_audio_without_pc_pin_is_reported) {
    Rig r;
    r.feed(audio_packet(5, 0, tone(0), 48000, "secret"));
    CHECK(r.mic.got.empty());
    CHECK_EQ(r.rx.snapshot().last_reject, PM_STATUS_PIN_MISSING);
}

TEST(rx_second_sender_is_busy_until_first_times_out) {
    Rig r;
    r.feed(audio_packet(1, 0, tone(0)), ep("192.168.1.20", 5000));
    r.feed(audio_packet(2, 0, tone(0)), ep("192.168.1.21", 5000));
    CHECK_EQ(r.mic.got.size(), 480u);  // the intruder's frame was not mixed in
    CHECK_EQ(r.rx.snapshot().last_reject, PM_STATUS_BUSY);
}

TEST(rx_new_session_from_same_phone_takes_over_after_timeout_and_flushes) {
    Rig r;
    r.feed(audio_packet(1, 0, tone(0)));
    std::this_thread::sleep_for(std::chrono::milliseconds(Receiver::kSessionTimeoutMs + 50));
    r.feed(audio_packet(2, 0, tone(0)));  // the phone restarted streaming
    CHECK_EQ(r.mic.got.size(), 960u);
    CHECK(r.mic.flushes >= 2);
}

TEST(rx_resamples_44k1_senders_to_48k) {
    Rig r;
    for (int s = 0; s < 100; ++s) r.feed(audio_packet(3, s, tone(s, 441), 44100));
    CHECK(std::abs(static_cast<long>(r.mic.got.size()) - 48000) < 10);
    CHECK_EQ(r.rx.snapshot().sample_rate, 44100u);
}

TEST(rx_accepts_legacy_v1_plaintext_and_refuses_v1_encrypted) {
    Rig r;
    std::vector<uint8_t> v1(18 + 960, 0);
    pm_w16(v1.data(), PM_MAGIC);
    v1[2] = 1;
    v1[3] = 0;
    pm_w16(v1.data() + 16, 960);
    r.feed(v1);
    CHECK_EQ(r.mic.got.size(), 480u);
    CHECK_EQ(r.rx.snapshot().sender_version, 1);

    Rig e;
    e.rx.set_passphrase("x");
    v1[3] = 0x80;
    e.feed(v1);
    CHECK(e.mic.got.empty());
    CHECK(e.rx.snapshot().legacy_encrypted_seen);
}

TEST(rx_mute_and_gain_apply) {
    Rig r;
    r.rx.set_mute(true);
    r.feed(audio_packet(1, 0, tone(0)));
    for (float x : r.mic.got) CHECK_EQ(x, 0.0f);
    Rig g;
    g.rx.set_gain_db(6.0f);
    g.feed(audio_packet(1, 0, tone(0)));
    const auto t = tone(0);
    CHECK_NEAR(g.mic.got[100], t[100] / 32768.0 * std::pow(10.0, 6.0 / 20.0), 1e-4);
}

TEST(rx_garbage_is_counted_not_played) {
    Rig r;
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    r.rx.handle_datagram(junk, sizeof junk, ep("10.0.0.1", 1));
    auto odd = audio_packet(1, 0, tone(0));
    odd.push_back(0);  // length mismatch
    r.feed(odd);
    const auto s = r.rx.snapshot();
    CHECK_EQ(s.malformed, 2u);
    CHECK(r.mic.got.empty());
}

TEST(rx_web_source_is_trusted_even_with_pin) {
    Rig r;
    r.rx.set_passphrase("pin");
    SourceId web;
    web.via = SourceId::Via::Web;
    web.conn_id = 1;
    const auto p = audio_packet(11, 0, tone(0));
    CHECK_EQ(r.rx.submit(web, p.data(), p.size(), true), PM_STATUS_OK);
    CHECK_EQ(r.mic.got.size(), 480u);
    r.rx.release_conn(1);
    CHECK(!r.rx.snapshot().connected);
}

TEST(rx_udp_discovery_and_status_feedback_over_loopback) {
    Receiver rx;
    CollectSink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    rx.set_host_name("test-pc");
    std::string err;
    CHECK(rx.start(0, &err));
    const uint16_t port = rx.port();

    net::UdpSocket phone;
    CHECK(phone.open(0, &err));
    const auto pc = ep("127.0.0.1", port);
    uint8_t buf[256];
    net::Endpoint from;

    // Discovery.
    uint8_t hello[PM_HELLO_LEN];
    pm_encode_hello(hello, 4242);
    CHECK(phone.send_to(pc, hello, sizeof hello));
    int n = phone.recv(buf, sizeof buf, &from, 2000);
    CHECK(n > 0);
    pm_announce a{};
    CHECK_EQ(pm_decode_announce(buf, static_cast<size_t>(n), &a), PM_OK);
    CHECK_EQ(a.nonce, 4242u);
    CHECK_EQ(a.audio_port, port);
    CHECK(std::string(a.name) == "test-pc");

    // Audio -> STATUS ok comes back to the sender.
    for (int s = 0; s < 5; ++s) {
        const auto p = audio_packet(77, s, tone(s));
        phone.send_to(pc, p.data(), p.size());
    }
    pm_status st{};
    bool got_ok = false;
    for (int i = 0; i < 20 && !got_ok; ++i) {
        n = phone.recv(buf, sizeof buf, &from, 200);
        if (n > 0 && pm_decode_status(buf, static_cast<size_t>(n), &st) == PM_OK && st.state == PM_STATUS_OK)
            got_ok = true;
    }
    CHECK(got_ok);
    CHECK_EQ(st.session_id, 77u);
    CHECK(st.received >= 1u);

    // Let the receiver finish the first five before changing policy.
    for (int i = 0; i < 100 && rx.snapshot().packets < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(rx.snapshot().packets, 5u);

    // Set a PIN: plaintext is now refused and the phone is told why.
    rx.set_passphrase("1234");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto p = audio_packet(77, 5, tone(5));
    phone.send_to(pc, p.data(), p.size());
    bool got_reject = false;
    for (int i = 0; i < 20 && !got_reject; ++i) {
        n = phone.recv(buf, sizeof buf, &from, 200);
        if (n > 0 && pm_decode_status(buf, static_cast<size_t>(n), &st) == PM_OK && st.state == PM_STATUS_PIN_REQUIRED)
            got_reject = true;
    }
    CHECK(got_reject);
    rx.stop();
    CHECK_EQ(mic.got.size(), 5u * 480);
}

TEST(rx_second_instance_cannot_bind_same_port) {
    Receiver a, b;
    std::string err;
    CHECK(a.start(0, &err));
    CHECK(!b.start(a.port(), &err));
    CHECK(!err.empty());
}
