// Protocol framing tests: packet layout, validation, discovery and frame prefixes.
#include <phonemic/wire.h>

#include <cstring>
#include <vector>

#include "test.hpp"

namespace {
const uint8_t kPayload[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

pm_audio_header hdr(uint32_t seq = 42, uint64_t ts = 1234567890ull, uint8_t flags = 0) {
    pm_audio_header h{};
    h.codec = PM_CODEC_PCM16;
    h.flags = flags;
    h.session_id = 0xCAFEBABE;
    h.seq = seq;
    h.timestamp_us = ts;
    h.sample_rate = 48000;
    return h;
}

std::vector<uint8_t> encode(const pm_audio_header& h, const uint8_t* p, size_t n) {
    std::vector<uint8_t> b(PM_AUDIO_HEADER_LEN + n);
    CHECK_EQ(pm_encode_audio(b.data(), b.size(), &h, p, n), static_cast<int>(b.size()));
    return b;
}

}  // namespace

TEST(wire_roundtrip_pcm16) {
    const auto b = encode(hdr(42, 1234567890ull), kPayload, sizeof kPayload);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_OK);
    CHECK_EQ(d.codec, PM_CODEC_PCM16);
    CHECK_EQ(d.session_id, 0xCAFEBABEu);
    CHECK_EQ(d.seq, 42u);
    CHECK_EQ(d.timestamp_us, 1234567890ull);
    CHECK_EQ(d.sample_rate, 48000u);
    CHECK_EQ(d.payload_len, sizeof kPayload);
    CHECK(std::memcmp(p, kPayload, sizeof kPayload) == 0);
}

TEST(wire_roundtrip_extremes) {
    const auto b = encode(hdr(0xFFFFFFFFu, ~0ull), kPayload, sizeof kPayload);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_OK);
    CHECK_EQ(d.seq, 0xFFFFFFFFu);
    CHECK_EQ(d.timestamp_us, ~0ull);
}

TEST(wire_empty_payload_is_legal_framing) {
    const auto b = encode(hdr(), nullptr, 0);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_OK);
    CHECK_EQ(d.payload_len, 0);
}

TEST(wire_encrypted_flag_roundtrips) {
    const auto b = encode(hdr(1, 2, PM_FLAG_ENCRYPTED), kPayload, 3);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_OK);
    CHECK(d.flags & PM_FLAG_ENCRYPTED);
}

TEST(wire_magic_is_ascii_pm_and_layout_is_little_endian) {
    const auto b = encode(hdr(0x04030201u, 0x0807060504030201ull), kPayload, sizeof kPayload);
    CHECK(b[0] == 'P' && b[1] == 'M');
    CHECK_EQ(b[2], 2);
    CHECK_EQ(b[3], PM_KIND_AUDIO);
    CHECK_EQ(b[6], sizeof kPayload);  // payload_len LE
    CHECK_EQ(b[8], 0xBE);             // session LE
    CHECK_EQ(b[12], 0x01);            // seq LE
    CHECK_EQ(b[16], 0x01);            // timestamp LE
    CHECK_EQ(pm_r32(b.data() + 24), 48000u);
}

TEST(wire_header_len_matches_layout) {
    CHECK_EQ(PM_AUDIO_HEADER_LEN, 2 + 1 + 1 + 1 + 1 + 2 + 4 + 4 + 8 + 4);
}

TEST(wire_rejects_too_short) {
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(nullptr, 0, &d, &p), PM_ERR_TOO_SHORT);
    const auto b = encode(hdr(), kPayload, sizeof kPayload);
    CHECK_EQ(pm_decode_audio(b.data(), PM_AUDIO_HEADER_LEN - 1, &d, &p), PM_ERR_TOO_SHORT);
}

TEST(wire_rejects_bad_magic) {
    auto b = encode(hdr(), kPayload, sizeof kPayload);
    b[0] ^= 0xFF;
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_BAD_MAGIC);
}

TEST(wire_rejects_unsupported_version) {
    auto b = encode(hdr(), kPayload, sizeof kPayload);
    b[2] = 3;
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_UNSUPPORTED_VERSION);
}

TEST(wire_rejects_unknown_codec_and_flags) {
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    auto b = encode(hdr(), kPayload, sizeof kPayload);
    b[4] = 5;
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_UNKNOWN_CODEC);
    b = encode(hdr(), kPayload, sizeof kPayload);
    b[5] = 0x02;  // only PM_FLAG_ENCRYPTED is defined
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_BAD_FIELD);
}

TEST(wire_rejects_bad_session_and_rate) {
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    auto h = hdr();
    h.session_id = 0;
    auto b = encode(h, kPayload, sizeof kPayload);
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_BAD_FIELD);
    h = hdr();
    h.sample_rate = 1000;
    b = encode(h, kPayload, sizeof kPayload);
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_BAD_FIELD);
}

TEST(wire_rejects_truncated_payload_and_trailing_garbage) {
    auto b = encode(hdr(), kPayload, sizeof kPayload);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b.data(), b.size() - 5, &d, &p), PM_ERR_LEN_MISMATCH);
    b.push_back(0);
    b.push_back(0);
    CHECK_EQ(pm_decode_audio(b.data(), b.size(), &d, &p), PM_ERR_LEN_MISMATCH);
}

TEST(wire_encode_rejects_small_output_and_huge_payload) {
    uint8_t small[PM_AUDIO_HEADER_LEN + 2];
    auto h = hdr();
    CHECK_EQ(pm_encode_audio(small, sizeof small, &h, kPayload, sizeof kPayload), PM_ERR_OUTPUT_TOO_SMALL);
    std::vector<uint8_t> big(PM_MAX_PAYLOAD + 1), out(PM_MAX_DATAGRAM + 16);
    CHECK_EQ(pm_encode_audio(out.data(), out.size(), &h, big.data(), big.size()), PM_ERR_PAYLOAD_TOO_LARGE);
}

TEST(wire_frame_prefix_is_little_endian_length) {
    uint8_t b[PM_FRAME_PREFIX_LEN];
    pm_write_frame_prefix(b, 0x0123);
    CHECK_EQ(b[0], 0x23);
    CHECK_EQ(b[1], 0x01);
}

TEST(wire_status_is_not_audio) {
    pm_status s{};
    uint8_t b[PM_STATUS_LEN];
    pm_encode_status(b, &s);
    uint8_t kind = 0xFF;
    CHECK_EQ(pm_peek(b, sizeof b, &kind), PM_OK);
    CHECK_EQ(kind, PM_KIND_STATUS);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b, sizeof b, &d, &p), PM_ERR_WRONG_KIND);
}

TEST(wire_hello_roundtrip) {
    uint8_t b[PM_HELLO_LEN];
    CHECK_EQ(pm_encode_hello(b, 0xDEADBEEF), PM_HELLO_LEN);
    uint32_t nonce = 0;
    CHECK_EQ(pm_decode_hello(b, sizeof b, &nonce), PM_OK);
    CHECK_EQ(nonce, 0xDEADBEEFu);
    CHECK_EQ(pm_decode_hello(b, sizeof b - 1, &nonce), PM_ERR_LEN_MISMATCH);
    pm_audio_header d{};
    const uint8_t* p = nullptr;
    CHECK_EQ(pm_decode_audio(b, sizeof b, &d, &p), PM_ERR_WRONG_KIND);
}

TEST(wire_announce_roundtrip_and_name_truncation) {
    pm_announce a{};
    a.nonce = 77;
    a.audio_port = 4010;
    a.flags = PM_ANNOUNCE_FLAG_PIN;
    std::strcpy(a.name, "desk-pc");
    uint8_t b[128];
    const int n = pm_encode_announce(b, sizeof b, &a);
    CHECK_EQ(n, PM_ANNOUNCE_MIN_LEN + 7);
    pm_announce d{};
    CHECK_EQ(pm_decode_announce(b, static_cast<size_t>(n), &d), PM_OK);
    CHECK_EQ(d.nonce, 77u);
    CHECK_EQ(d.audio_port, 4010);
    CHECK_EQ(d.flags, PM_ANNOUNCE_FLAG_PIN);
    CHECK(std::string(d.name) == "desk-pc");
    // A lying name_len is rejected, never over-read.
    b[11] = 50;
    CHECK(pm_decode_announce(b, static_cast<size_t>(n), &d) != PM_OK);
}

TEST(wire_status_roundtrip) {
    pm_status s{};
    s.state = PM_STATUS_BUSY;
    s.buffer_ms = 42;
    s.session_id = 9;
    s.received = 1000;
    s.lost = 3;
    uint8_t b[PM_STATUS_LEN];
    CHECK_EQ(pm_encode_status(b, &s), PM_STATUS_LEN);
    pm_status d{};
    CHECK_EQ(pm_decode_status(b, sizeof b, &d), PM_OK);
    CHECK_EQ(d.state, PM_STATUS_BUSY);
    CHECK_EQ(d.buffer_ms, 42);
    CHECK_EQ(d.session_id, 9u);
    CHECK_EQ(d.received, 1000u);
    CHECK_EQ(d.lost, 3u);
}

TEST(wire_decoder_never_crashes_on_garbage) {
    // Deterministic fuzz: random lengths/bytes, plus mutations of a valid packet.
    uint32_t x = 12345;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
    const auto valid = encode(hdr(), kPayload, sizeof kPayload);
    std::vector<uint8_t> b;
    for (int i = 0; i < 20000; ++i) {
        if (i % 2) {
            b.resize(rnd() % 64);
            for (auto& c : b) c = static_cast<uint8_t>(rnd());
            if (b.size() >= 2) { b[0] = 'P'; b[1] = 'M'; }
        } else {
            b = valid;
            b[rnd() % b.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8));
            b.resize(b.size() - (rnd() % 3));
        }
        pm_audio_header d{};
        const uint8_t* p = nullptr;
        if (pm_decode_audio(b.data(), b.size(), &d, &p) == PM_OK) {
            CHECK(p >= b.data() && p + d.payload_len <= b.data() + b.size());
        }
        pm_status s{};
        pm_decode_status(b.data(), b.size(), &s);
    }
}
