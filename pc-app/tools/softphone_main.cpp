// phonemic-softphone — emulates the Android sender on the PC.
//
// Streams a synthetic tone with the real v2 protocol (optionally encrypted),
// can discover PCs on the LAN, simulate packet loss / reordering, and prints
// the STATUS feedback the receiver sends back. A dev/test tool.
#include <phonemic/crypto.h>
#include <phonemic/wire.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "phonemic/net.hpp"
#include "phonemic/opus_codec.hpp"
#include "phonemic/util.hpp"

using namespace phonemic;

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }

const char* status_text(uint8_t s) {
    switch (s) {
        case PM_STATUS_OK: return "ok";
        case PM_STATUS_PIN_REQUIRED: return "PC requires a PIN";
        case PM_STATUS_PIN_MISSING: return "PC has no PIN set";
        case PM_STATUS_PIN_WRONG: return "PIN mismatch";
        case PM_STATUS_BUSY: return "PC busy (another device)";
        case PM_STATUS_UNSUPPORTED: return "unsupported format";
    }
    return "?";
}

bool discover(net::UdpSocket& s, uint16_t port, net::Endpoint* found) {
    s.set_broadcast(true);
    const uint32_t nonce = random_u32_nonzero();
    uint8_t hello[PM_HELLO_LEN];
    pm_encode_hello(hello, nonce);
    for (const char* ip : {"255.255.255.255", "127.0.0.1"}) {
        net::Endpoint e;
        net::Endpoint::parse(ip, port, &e);
        s.send_to(e, hello, sizeof hello);
    }
    const uint64_t end = mono_ms() + 1500;
    bool any = false;
    uint8_t buf[256];
    while (mono_ms() < end) {
        net::Endpoint from;
        const int n = s.recv(buf, sizeof buf, &from, 100);
        pm_announce a{};
        if (n > 0 && pm_decode_announce(buf, static_cast<size_t>(n), &a) == PM_OK && a.nonce == nonce) {
            std::printf("found \"%s\" at %s port %u%s%s\n", a.name, from.ip_string().c_str(), a.audio_port,
                        (a.flags & PM_ANNOUNCE_FLAG_PIN) ? " [PIN]" : "", (a.flags & PM_ANNOUNCE_FLAG_BUSY) ? " [busy]" : "");
            if (!any) {
                *found = from;
                found->port = a.audio_port;
                any = true;
            }
        }
    }
    return any;
}
}  // namespace

int main(int argc, char** argv) {
    std::string target = "127.0.0.1", pin;
    uint16_t port = 4010;
    if (const char* e = std::getenv("PHONEMIC_PORT")) port = static_cast<uint16_t>(std::atoi(e));
    uint32_t rate = 48000;
    double freq = 440, loss = 0, reorder = 0, seconds = 0;
    bool do_discover = false, wallclock = false, opus = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--pin") pin = next();
        else if (a == "--rate") rate = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--freq") freq = std::stod(next());
        else if (a == "--loss") loss = std::stod(next()) / 100.0;
        else if (a == "--reorder") reorder = std::stod(next()) / 100.0;
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--discover") do_discover = true;
        else if (a == "--wallclock") wallclock = true;
        else if (a == "--opus") opus = true;
        else if (a[0] != '-') target = a;
        else {
            std::puts("usage: phonemic-softphone [PC_IP] [--discover] [--port N] [--pin TEXT] [--rate HZ]\n"
                      "                          [--freq HZ] [--loss PCT] [--reorder PCT] [--seconds S] [--opus]\n"
                      "                          [--wallclock (timestamps for receiver --measure)]");
            return a == "--help" ? 0 : 2;
        }
    }
    std::signal(SIGINT, on_signal);

    net::UdpSocket sock;
    std::string err;
    if (!sock.open(0, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    net::Endpoint dest;
    if (do_discover) {
        if (!discover(sock, port, &dest)) { std::fprintf(stderr, "no PhoneMic PC answered\n"); return 1; }
    } else if (!net::Endpoint::parse(target, port, &dest)) {
        std::fprintf(stderr, "bad IP: %s\n", target.c_str());
        return 2;
    }

    const uint32_t session = random_u32_nonzero();
    uint8_t key[32];
    if (!pin.empty() && pm_derive_key(pin.data(), pin.size(), session, key) != 0) return 1;
    if (opus && rate != 48000) {
        std::fprintf(stderr, "--opus needs --rate 48000\n");
        return 2;
    }
    OpusVoiceEncoder encoder(48000);
    if (opus && !encoder.ok()) { std::fprintf(stderr, "opus encoder init failed\n"); return 1; }
    const size_t frame = rate / 100;
    std::printf("softphone -> %s  (%u Hz, 10 ms frames, %.0f Hz tone, %s%s, session %08x)\n", dest.to_string().c_str(),
                rate, freq, opus ? "Opus" : "PCM", pin.empty() ? "" : ", encrypted", session);

    std::mt19937 rng(session);
    std::uniform_real_distribution<double> uni(0, 1);
    std::vector<uint8_t> pcm(frame * 2), pkt(PM_MAX_DATAGRAM), held, enc(1500);
    std::vector<int16_t> samples(frame);
    double phase = 0;
    uint32_t seq = 0, sent = 0;
    auto next_tick = std::chrono::steady_clock::now();
    const auto start = next_tick;
    uint8_t last_state = 255;
    while (!g_quit.load()) {
        if (seconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::duration<double>(seconds)) break;
        for (size_t i = 0; i < frame; ++i) {
            const int16_t v = static_cast<int16_t>(std::sin(phase) * 8000);
            samples[i] = v;
            phase += 6.283185307179586 * freq / rate;
            pcm[2 * i] = static_cast<uint8_t>(v & 0xFF);
            pcm[2 * i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
        }
        const uint8_t* body = pcm.data();
        size_t body_len = pcm.size();
        if (opus) {
            const int n = encoder.encode(samples.data(), static_cast<int>(frame), enc.data(), static_cast<int>(enc.size()));
            if (n <= 0) { std::fprintf(stderr, "opus encode failed\n"); return 1; }
            body = enc.data();
            body_len = static_cast<size_t>(n);
        }
        pm_audio_header h{};
        h.codec = opus ? PM_CODEC_OPUS : PM_CODEC_PCM16;
        h.session_id = session;
        h.seq = seq++;
        h.timestamp_us = wallclock ? wall_us() : mono_us();
        h.sample_rate = rate;
        size_t n;
        if (pin.empty()) {
            n = static_cast<size_t>(pm_encode_audio(pkt.data(), pkt.size(), &h, body, body_len));
        } else {
            h.flags = PM_FLAG_ENCRYPTED;
            h.payload_len = static_cast<uint16_t>(body_len + PM_TAG_LEN);
            pm_write_audio_header(pkt.data(), &h);
            pm_seal(key, pkt.data(), PM_AUDIO_HEADER_LEN, session, h.seq, h.timestamp_us, body, body_len,
                    pkt.data() + PM_AUDIO_HEADER_LEN);
            n = PM_AUDIO_HEADER_LEN + h.payload_len;
        }
        if (uni(rng) >= loss) {
            if (held.empty() && uni(rng) < reorder) {
                held.assign(pkt.begin(), pkt.begin() + static_cast<long>(n));  // send after the next one
            } else {
                sock.send_to(dest, pkt.data(), n);
                if (!held.empty()) { sock.send_to(dest, held.data(), held.size()); held.clear(); }
                ++sent;
            }
        }
        // Drain feedback.
        uint8_t buf[64];
        net::Endpoint from;
        int r;
        while ((r = sock.recv(buf, sizeof buf, &from, 0)) > 0) {
            pm_status st{};
            if (pm_decode_status(buf, static_cast<size_t>(r), &st) == PM_OK && st.state != last_state) {
                std::printf("PC says: %s (received %u, lost %u, buffer %u ms)\n", status_text(st.state), st.received,
                            st.lost, st.buffer_ms);
                std::fflush(stdout);
                last_state = st.state;
            }
        }
        next_tick += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_tick);
    }
    std::printf("sent %u packets\n", sent);
    return 0;
}
