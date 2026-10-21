// Opus on the wire: codec round trip, receiver integration, FEC recovery.
#include "phonemic/opus_codec.hpp"

#include <phonemic/wire.h>

#include <opus.h>

#include <cmath>
#include <vector>

#include "phonemic/receiver.hpp"
#include "test.hpp"

using namespace phonemic;

namespace {
std::vector<int16_t> voice_frame(int idx) {
    // A voice-like signal: 180 Hz fundamental with harmonics.
    std::vector<int16_t> f(480);
    for (int i = 0; i < 480; ++i) {
        const double t = (idx * 480 + i) / 48000.0;
        double v = 0;
        for (int k = 1; k <= 6; ++k) v += std::sin(6.283185307 * 180.0 * k * t) / k;
        f[static_cast<size_t>(i)] = static_cast<int16_t>(v * 5000);
    }
    return f;
}

size_t zero_crossings(const std::vector<float>& s) {
    size_t c = 0;
    for (size_t i = 1; i < s.size(); ++i) c += (s[i - 1] < 0) != (s[i] < 0);
    return c;
}

struct Sink : AudioSinkIface {
    std::vector<float> got;
    void write(const float* s, size_t n) override { got.insert(got.end(), s, s + n); }
};

std::vector<uint8_t> opus_packet(OpusVoiceEncoder& enc, uint32_t seq, std::vector<uint8_t>* raw = nullptr) {
    const auto pcm = voice_frame(static_cast<int>(seq));
    uint8_t body[1500];
    const int n = enc.encode(pcm.data(), 480, body, sizeof body);
    if (raw) raw->assign(body, body + (n > 0 ? n : 0));
    pm_audio_header h{};
    h.codec = PM_CODEC_OPUS;
    h.session_id = 42;
    h.seq = seq;
    h.timestamp_us = 1000000ull + seq * 10000ull;
    h.sample_rate = 48000;
    std::vector<uint8_t> pkt(PM_AUDIO_HEADER_LEN + static_cast<size_t>(n));
    pm_encode_audio(pkt.data(), pkt.size(), &h, body, static_cast<size_t>(n));
    return pkt;
}
}  // namespace

TEST(opus_roundtrip_keeps_pitch_and_compresses) {
    OpusVoiceEncoder enc(48000);
    OpusVoiceDecoder dec;
    CHECK(enc.ok());
    std::vector<float> all, frame;
    size_t bytes = 0;
    for (int i = 0; i < 100; ++i) {
        const auto pcm = voice_frame(i);
        uint8_t pkt[1500];
        const int n = enc.encode(pcm.data(), 480, pkt, sizeof pkt);
        CHECK(n > 0);
        bytes += static_cast<size_t>(n);
        CHECK(dec.decode(pkt, static_cast<size_t>(n), frame));
        CHECK_EQ(frame.size(), 480u);
        all.insert(all.end(), frame.begin(), frame.end());
    }
    // ~48 kbps instead of 768 kbps PCM.
    CHECK(bytes * 8 < 70000);
    // Same fundamental: compare against the source's crossings (skip codec warm-up).
    std::vector<float> src;
    for (int i = 0; i < 100; ++i)
        for (int16_t v : voice_frame(i)) src.push_back(v / 32768.0f);
    const std::vector<float> a(all.begin() + 4800, all.end()), b(src.begin() + 4800, src.end());
    const double ratio = static_cast<double>(zero_crossings(a)) / static_cast<double>(zero_crossings(b));
    CHECK(ratio > 0.9 && ratio < 1.1);
}

TEST(opus_receiver_decodes_stream) {
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    OpusVoiceEncoder enc(48000);
    SourceId from;
    from.conn_id = 1;
    for (uint32_t s = 0; s < 50; ++s) {
        const auto p = opus_packet(enc, s);
        rx.submit(from, p.data(), p.size());
    }
    CHECK_EQ(mic.got.size(), 50u * 480);
    const auto snap = rx.snapshot();
    CHECK(snap.opus);
    CHECK_EQ(snap.sample_rate, 48000u);
    CHECK_EQ(snap.lost, 0u);
}

TEST(opus_lost_packet_is_rebuilt_from_fec) {
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    OpusVoiceEncoder enc(48000);
    SourceId from;
    from.conn_id = 1;
    // libopus decides per frame whether to add FEC (LBRR) data; drop a packet
    // whose successor carries it, as happens on a real lossy link.
    std::vector<std::vector<uint8_t>> pkts, raws;
    for (uint32_t s = 0; s < 80; ++s) {
        std::vector<uint8_t> raw;
        pkts.push_back(opus_packet(enc, s, &raw));
        raws.push_back(raw);
    }
    int drop = -1;
    for (int s = 20; s + 1 < 80 && drop < 0; ++s)
        if (opus_packet_has_lbrr(raws[s + 1].data(), static_cast<opus_int32>(raws[s + 1].size())) == 1) drop = s;
    CHECK(drop >= 0);  // voice at 48 kbps (hybrid mode) does carry FEC
    for (int s = 0; s < 80; ++s)
        if (s != drop) rx.submit(from, pkts[s].data(), pkts[s].size());
    const auto snap = rx.snapshot();
    CHECK_EQ(snap.lost, 1u);
    CHECK_EQ(snap.fec_recovered, 1u);       // rebuilt from the next packet
    CHECK_EQ(mic.got.size(), 80u * 480);    // timeline intact
}

TEST(opus_corrupt_packet_is_concealed_not_fatal) {
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    OpusVoiceEncoder enc(48000);
    SourceId from;
    from.conn_id = 1;
    for (uint32_t s = 0; s < 10; ++s) {
        auto p = opus_packet(enc, s);
        if (s == 5) {
            // An invalid Opus packet (code 3 with zero frames).
            p.resize(PM_AUDIO_HEADER_LEN + 1);
            p[PM_AUDIO_HEADER_LEN] = 0xFF;
            pm_w16(p.data() + 6, 1);
        }
        rx.submit(from, p.data(), p.size());
    }
    CHECK_EQ(mic.got.size(), 10u * 480);
    for (float x : mic.got) CHECK(std::isfinite(x));
}
