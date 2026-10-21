// End-to-end tests of the receive pipeline: packets in -> processed audio out.
#include "phonemic/receiver.hpp"

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
                                  uint32_t rate = 48000) {
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
    std::vector<uint8_t> out(PM_AUDIO_HEADER_LEN + payload.size());
    pm_encode_audio(out.data(), out.size(), &h, payload.data(), payload.size());
    return out;
}

std::vector<int16_t> tone(int seq, size_t n = 480) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<int16_t>(8000 * std::sin(6.2831853 * 440.0 * (seq * static_cast<double>(n) + i) / 48000.0));
    return v;
}

SourceId phone(uint64_t conn) {
    SourceId s;
    s.conn_id = conn;
    s.name = "Pixel " + std::to_string(conn);
    return s;
}

struct Rig {
    Receiver rx;
    CollectSink mic, mon;
    Rig() {
        rx.set_voice_focus(false);  // keep the signal bit-predictable
        rx.set_mic_sink(&mic);
        rx.set_monitor_sink(&mon);
    }
    uint8_t feed(const std::vector<uint8_t>& p, uint64_t conn = 1) { return rx.submit(phone(conn), p.data(), p.size()); }
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
    CHECK(snap.peer == "Pixel 1");
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

TEST(rx_second_phone_is_busy_until_first_disconnects_or_times_out) {
    Rig r;
    CHECK_EQ(r.feed(audio_packet(1, 0, tone(0)), 1), PM_STATUS_OK);
    CHECK_EQ(r.feed(audio_packet(2, 0, tone(0)), 2), PM_STATUS_BUSY);
    CHECK_EQ(r.mic.got.size(), 480u);  // the intruder's frame was not mixed in
    CHECK_EQ(r.rx.snapshot().last_reject, PM_STATUS_BUSY);
    r.rx.release_conn(1);
    CHECK(!r.rx.snapshot().connected);
    CHECK_EQ(r.feed(audio_packet(2, 0, tone(0)), 2), PM_STATUS_OK);

    Rig t;
    t.feed(audio_packet(1, 0, tone(0)), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(Receiver::kSessionTimeoutMs + 50));
    CHECK_EQ(t.feed(audio_packet(2, 0, tone(0)), 2), PM_STATUS_OK);  // the silent owner lost the mic
}

TEST(rx_new_session_on_same_connection_takes_over_and_flushes) {
    Rig r;
    r.feed(audio_packet(1, 0, tone(0)));
    r.feed(audio_packet(1, 1, tone(1)));
    CHECK_EQ(r.feed(audio_packet(2, 0, tone(0))), PM_STATUS_OK);  // the phone restarted streaming
    CHECK_EQ(r.mic.got.size(), 1440u);
    CHECK(r.mic.flushes >= 2);
    CHECK_EQ(r.rx.snapshot().session_received, 1u);
}

TEST(rx_resamples_44k1_senders_to_48k) {
    Rig r;
    for (int s = 0; s < 100; ++s) r.feed(audio_packet(3, s, tone(s, 441), 44100));
    CHECK(std::abs(static_cast<long>(r.mic.got.size()) - 48000) < 10);
    CHECK_EQ(r.rx.snapshot().sample_rate, 44100u);
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
    CHECK_EQ(r.rx.submit(phone(1), junk, sizeof junk), PM_STATUS_UNSUPPORTED);
    auto odd = audio_packet(1, 0, tone(0));
    odd.push_back(0);  // length mismatch
    r.feed(odd);
    const auto s = r.rx.snapshot();
    CHECK_EQ(s.malformed, 2u);
    CHECK(r.mic.got.empty());
}

TEST(rx_status_reports_the_active_session_only) {
    Rig r;
    for (int s = 0; s < 4; ++s) r.feed(audio_packet(9, s, tone(s)), 1);
    SourceId me = phone(1);
    me.session = 9;
    const pm_status st = r.rx.make_status(me, PM_STATUS_OK);
    CHECK_EQ(st.session_id, 9u);
    CHECK_EQ(st.received, 4u);
    CHECK_EQ(st.lost, 0u);
    SourceId other = phone(2);
    other.session = 5;
    const pm_status busy = r.rx.make_status(other, PM_STATUS_BUSY);
    CHECK_EQ(busy.state, PM_STATUS_BUSY);
    CHECK_EQ(busy.received, 0u);
}
