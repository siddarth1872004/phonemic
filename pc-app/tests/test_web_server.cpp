// Integration test: real TLS + HTTP + WebSocket against the built-in server.
#include "phonemic/web_server.hpp"

#include <phonemic/wire.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "phonemic/util.hpp"
#include "phonemic/websocket.hpp"
#include "test.hpp"

using namespace phonemic;

namespace {
struct Sink : AudioSinkIface {
    std::vector<float> got;
    void write(const float* s, size_t n) override { got.insert(got.end(), s, s + n); }
};

// A small blocking TLS client. Every mbedTLS call holds the shared lock.
struct Client {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;

    template <typename F>
    int locked(F f) {
        std::lock_guard<std::mutex> lk(web_tls_mutex());
        return f();
    }
    explicit Client(uint16_t port) {
        locked([&] {
            mbedtls_net_init(&net);
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&conf);
            mbedtls_entropy_init(&ent);
            mbedtls_ctr_drbg_init(&drbg);
            if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, nullptr, 0)) return 1;
            if (mbedtls_net_connect(&net, "127.0.0.1", std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP)) return 1;
            mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  // self-signed, like a browser after "proceed"
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            mbedtls_ssl_setup(&ssl, &conf);
            mbedtls_net_set_nonblock(&net);
            mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);
            return 0;
        });
        ok = io([&] { return mbedtls_ssl_handshake(&ssl); }) == 0;
    }
    ~Client() {
        locked([&] {
            mbedtls_net_free(&net);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&ent);
            return 0;
        });
    }
    template <typename F>
    int io(F f, int ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            const int r = locked(f);
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return r;
            if (std::chrono::steady_clock::now() > end) return -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    bool send(const std::string& s) { return send(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    bool send(const uint8_t* p, size_t n) {
        while (n) {
            const int r = io([&] { return mbedtls_ssl_write(&ssl, p, n); });
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }
    std::string recv_until(const std::string& needle, int ms = 5000) {
        std::string got;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        uint8_t buf[4096];
        while (got.find(needle) == std::string::npos && std::chrono::steady_clock::now() < end) {
            const int r = io([&] { return mbedtls_ssl_read(&ssl, buf, sizeof buf); }, 300);
            if (r > 0) got.append(reinterpret_cast<char*>(buf), static_cast<size_t>(r));
            else if (r != -1) break;
        }
        return got;
    }
};

std::vector<uint8_t> masked(uint8_t opcode, const std::vector<uint8_t>& p) {
    std::vector<uint8_t> f{static_cast<uint8_t>(0x80 | opcode)};
    if (p.size() < 126) f.push_back(static_cast<uint8_t>(0x80 | p.size()));
    else { f.push_back(0x80 | 126); f.push_back(static_cast<uint8_t>(p.size() >> 8)); f.push_back(static_cast<uint8_t>(p.size())); }
    const uint8_t m[4] = {9, 8, 7, 6};
    f.insert(f.end(), m, m + 4);
    for (size_t i = 0; i < p.size(); ++i) f.push_back(p[i] ^ m[i & 3]);
    return f;
}

void use_temp_config_dir() {
    const auto dir = std::filesystem::temp_directory_path() / ("phonemic-web-" + random_token(8));
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("APPDATA", dir.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
    setenv("HOME", dir.string().c_str(), 1);
#endif
}
}  // namespace

TEST(web_serves_page_rejects_bad_code_and_streams_audio) {
    use_temp_config_dir();
    Receiver rx;
    Sink mic;
    rx.set_mic_sink(&mic);
    rx.set_voice_focus(false);
    rx.set_passphrase("udp-pin");  // must not block the (already authenticated) browser path
    WebServer web(&rx, nullptr);
    std::string err;
    uint16_t port = 0;
    for (uint16_t p = 18443; p < 18463 && !port; ++p)
        if (web.start(p, "goodcode", &err)) port = p;
    CHECK(port != 0);

    {
        Client c(port);
        CHECK(c.ok);
        c.send("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const auto r = c.recv_until("</html>");
        CHECK(r.rfind("HTTP/1.1 200", 0) == 0);
        CHECK(r.find("Content-Security-Policy") != std::string::npos);
        CHECK(r.find("PhoneMic") != std::string::npos);
    }
    {
        Client c(port);
        CHECK(c.ok);
        c.send("GET /ws?t=badcode HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        CHECK(c.recv_until("\r\n\r\n").rfind("HTTP/1.1 403", 0) == 0);
    }
    {
        Client c(port);
        CHECK(c.ok);
        c.send("GET /ws?t=goodcode HTTP/1.1\r\nHost: localhost:" + std::to_string(port) +
               "\r\nOrigin: https://localhost:" + std::to_string(port) +
               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        const auto r = c.recv_until("\r\n\r\n");
        CHECK(r.rfind("HTTP/1.1 101", 0) == 0);
        CHECK(r.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

        for (uint32_t s = 0; s < 3; ++s) {
            pm_audio_header h{};
            h.codec = PM_CODEC_PCM16;
            h.session_id = 99;
            h.seq = s;
            h.sample_rate = 48000;
            std::vector<uint8_t> pkt(PM_AUDIO_HEADER_LEN + 960, 0);
            pm_encode_audio(pkt.data(), pkt.size(), &h, pkt.data() + PM_AUDIO_HEADER_LEN, 960);
            const auto f = masked(ws::kBinary, pkt);
            CHECK(c.send(f.data(), f.size()));
        }
        const auto status = c.recv_until("}");
        CHECK(status.find("\"state\":0") != std::string::npos);
        for (int i = 0; i < 300 && rx.snapshot().packets < 3; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    web.stop();
    CHECK_EQ(mic.got.size(), 3u * 480);
}
