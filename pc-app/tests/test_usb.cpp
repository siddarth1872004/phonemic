// USB mode: framed TCP on 127.0.0.1 (what `adb reverse` connects to).
#include "phonemic/usb.hpp"

#include <phonemic/wire.h>

#include <chrono>
#include <cmath>
#include <thread>

#include "phonemic/net.hpp"
#include "test.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace phonemic;

namespace {
struct Sink : AudioSinkIface {
    std::vector<float> got;
    void write(const float* s, size_t n) override { got.insert(got.end(), s, s + n); }
};

std::vector<uint8_t> framed_audio(uint32_t session, uint32_t seq) {
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
    std::vector<uint8_t> out(2 + PM_AUDIO_HEADER_LEN + pcm.size());
    const int n = pm_encode_audio(out.data() + 2, out.size() - 2, &h, pcm.data(), pcm.size());
    out[0] = static_cast<uint8_t>(n & 0xFF);
    out[1] = static_cast<uint8_t>(n >> 8);
    return out;
}

#ifdef _WIN32
using sock_t = SOCKET;
void close_s(sock_t s) { closesocket(s); }
#else
using sock_t = int;
void close_s(sock_t s) { ::close(s); }
#endif

sock_t connect_local(uint16_t port) {
    net::init();
    sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return s;
}

// Read framed STATUS packets until one has `state` (or time runs out).
bool wait_status(sock_t s, uint8_t state, pm_status* got) {
    FrameSplitter fs(256);
    std::vector<uint8_t> f;
    uint8_t buf[512];
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{0, 100000};
        if (::select(static_cast<int>(s + 1), &set, nullptr, nullptr, &tv) <= 0) continue;
        const auto n = ::recv(s, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (n <= 0) return false;
        fs.feed(buf, static_cast<size_t>(n));
        while (fs.next(&f))
            if (pm_decode_status(f.data(), f.size(), got) == PM_OK && got->state == state) return true;
    }
    return false;
}
}  // namespace

TEST(usb_tcp_stream_plays_and_reports_status) {
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    UsbServer usb(&rx, nullptr);
    std::string err;
    CHECK(usb.start(0, &err));
    const sock_t s = connect_local(usb.port());
    for (uint32_t i = 0; i < 5; ++i) {
        const auto f = framed_audio(77, i);
        CHECK(::send(s, reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0) ==
              static_cast<int>(f.size()));
    }
    pm_status st{};
    CHECK(wait_status(s, PM_STATUS_OK, &st));
    CHECK_EQ(st.session_id, 77u);
    for (int i = 0; i < 100 && rx.snapshot().packets < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(mic.got.size(), 5u * 480);
    CHECK(rx.snapshot().via == SourceId::Via::Usb);

    // The cable is a trusted link: a Wi-Fi PIN doesn't lock USB out.
    rx.set_passphrase("1234");
    const auto f = framed_audio(77, 5);
    ::send(s, reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0);
    for (int i = 0; i < 100 && rx.snapshot().packets < 6; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(rx.snapshot().packets, 6u);
    close_s(s);
    for (int i = 0; i < 100 && rx.snapshot().connected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!rx.snapshot().connected);  // the closed connection released the mic
    usb.stop();
}

TEST(usb_listener_is_loopback_only) {
    Receiver rx;
    UsbServer usb(&rx, nullptr);
    std::string err;
    CHECK(usb.start(0, &err));
    // Connecting via a LAN address of this machine must fail.
    for (const auto& a : net::local_addresses()) {
        net::init();
        sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(usb.port());
        inet_pton(AF_INET, a.ip.c_str(), &sa.sin_addr);
        CHECK(::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0);
        close_s(s);
    }
}
