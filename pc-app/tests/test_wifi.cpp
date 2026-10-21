// Wi-Fi mode: UDP discovery, audio and STATUS through WifiServer.
#include "phonemic/wifi.hpp"

#include <phonemic/wire.h>

#include <chrono>
#include <cmath>
#include <thread>

#include "test.hpp"

using namespace phonemic;

namespace {
struct Sink : AudioSinkIface {
    std::vector<float> got;
    void write(const float* s, size_t n) override { got.insert(got.end(), s, s + n); }
};

std::vector<uint8_t> udp_audio(uint32_t session, uint32_t seq) {
    pm_audio_header h{};
    h.codec = PM_CODEC_PCM16;
    h.session_id = session;
    h.seq = seq;
    h.timestamp_us = 1000000ull + seq * 10000ull;
    h.sample_rate = 48000;
    std::vector<uint8_t> pcm(960);
    for (size_t i = 0; i < 480; ++i) {
        const auto v = static_cast<int16_t>(4000 * std::sin(0.05 * static_cast<double>(seq * 480 + i)));
        pcm[2 * i] = static_cast<uint8_t>(v & 0xFF);
        pcm[2 * i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
    }
    std::vector<uint8_t> out(PM_AUDIO_HEADER_LEN + pcm.size());
    pm_encode_audio(out.data(), out.size(), &h, pcm.data(), pcm.size());
    return out;
}

// Receive datagrams until a STATUS with `state` arrives (or time runs out).
bool wait_status(net::UdpSocket& s, uint8_t state, pm_status* st) {
    uint8_t buf[256];
    net::Endpoint from;
    for (int i = 0; i < 20; ++i) {
        const int n = s.recv(buf, sizeof buf, &from, 200);
        if (n > 0 && pm_decode_status(buf, static_cast<size_t>(n), st) == PM_OK && st->state == state) return true;
    }
    return false;
}
}  // namespace

TEST(wifi_discovery_audio_and_pin_policy) {
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    WifiServer wifi(&rx, nullptr);
    wifi.set_host_name("test-pc");
    std::string err;
    CHECK(wifi.start(0, &err));

    net::UdpSocket phone;
    CHECK(phone.open(0, &err));
    net::Endpoint pc;
    CHECK(net::Endpoint::parse("127.0.0.1", wifi.port(), &pc));
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
    CHECK_EQ(a.audio_port, wifi.port());
    CHECK_EQ(a.flags & PM_ANNOUNCE_FLAG_PIN, 0);
    CHECK(std::string(a.name) == "test-pc");

    // Audio -> STATUS ok comes back to the sender.
    for (uint32_t s = 0; s < 5; ++s) {
        const auto p = udp_audio(77, s);
        phone.send_to(pc, p.data(), p.size());
    }
    pm_status st{};
    CHECK(wait_status(phone, PM_STATUS_OK, &st));
    CHECK_EQ(st.session_id, 77u);
    for (int i = 0; i < 100 && rx.snapshot().packets < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(rx.snapshot().packets, 5u);
    CHECK(rx.snapshot().via == SourceId::Via::Wifi);

    // Set a PIN: discovery says so, plaintext is refused and the phone is told why.
    rx.set_passphrase("1234");
    pm_encode_hello(hello, 4343);
    phone.send_to(pc, hello, sizeof hello);
    bool pin_flag = false;
    for (int i = 0; i < 10 && !pin_flag; ++i) {
        n = phone.recv(buf, sizeof buf, &from, 200);
        if (n > 0 && pm_decode_announce(buf, static_cast<size_t>(n), &a) == PM_OK && a.nonce == 4343u)
            pin_flag = (a.flags & PM_ANNOUNCE_FLAG_PIN) != 0;
    }
    CHECK(pin_flag);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto p = udp_audio(77, 5);
    phone.send_to(pc, p.data(), p.size());
    CHECK(wait_status(phone, PM_STATUS_PIN_REQUIRED, &st));
    wifi.stop();
    CHECK_EQ(mic.got.size(), 5u * 480);
}

TEST(wifi_second_server_cannot_bind_same_port) {
    Receiver rx;
    WifiServer a(&rx, nullptr), b(&rx, nullptr);
    std::string err;
    CHECK(a.start(0, &err));
    CHECK(!b.start(a.port(), &err));
    CHECK(!err.empty());
}
