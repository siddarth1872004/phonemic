#include "phonemic/web_server.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <monocypher.h>

#include <array>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <qrcodegen.hpp>

#include "phonemic/net.hpp"
#include "phonemic/settings.hpp"
#include "phonemic/websocket.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace phonemic {

namespace web_assets {
struct Asset {
    const char* path;
    const char* mime;
    const uint8_t* data;
    size_t size;
};
extern const Asset kAssets[];
}  // namespace web_assets

namespace {

constexpr int kMaxConnections = 8;
constexpr size_t kMaxHeader = 8192;
constexpr int kHttpTimeoutMs = 15000;
constexpr int kWsIdleTimeoutMs = 15000;

// mbedTLS 3.6 routes TLS 1.3 crypto through PSA, whose global state is not
// thread-safe unless MBEDTLS_THREADING_C is configured. All TLS calls therefore
// hold this lock; sockets are non-blocking, so waiting happens in select()
// outside it and connections never stall each other.
std::mutex g_tls;

const web_assets::Asset* find_asset(const std::string& path) {
    const std::string p = (path == "/") ? "/index.html" : path;
    for (const auto* a = web_assets::kAssets; a->path; ++a)
        if (p == a->path) return a;
    return nullptr;
}

bool wait_fd(int fd, bool for_write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = ::select(fd + 1, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv);
    return r > 0;
}

int rng_seed(mbedtls_ctr_drbg_context* drbg, mbedtls_entropy_context* ent) {
    static const char pers[] = "phonemic-web";
    return mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, ent,
                                 reinterpret_cast<const unsigned char*>(pers), sizeof pers - 1);
}

std::string mbed_err(int rc) {
    char buf[160] = {0};
    mbedtls_strerror(rc, buf, sizeof buf);
    return buf;
}

// One TLS connection. All mbedtls_* calls happen under g_tls.
struct Tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    std::atomic<bool>* stop;

    Tls(int fd, std::atomic<bool>* stop_flag) : stop(stop_flag) {
        std::lock_guard<std::mutex> lk(g_tls);
        mbedtls_net_init(&net);
        net.fd = fd;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&pk);
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Tls() {
        std::lock_guard<std::mutex> lk(g_tls);
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&net);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&crt);
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&ent);
    }

    bool setup(const std::string& cert_pem, const std::string& key_pem) {
        std::lock_guard<std::mutex> lk(g_tls);
        if (rng_seed(&drbg, &ent) != 0) return false;
        if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert_pem.c_str()),
                                   cert_pem.size() + 1) != 0)
            return false;
        if (mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(key_pem.c_str()),
                                 key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg) != 0)
            return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return false;
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        if (mbedtls_ssl_conf_own_cert(&conf, &crt, &pk) != 0) return false;
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        mbedtls_net_set_nonblock(&net);
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);
        return true;
    }

    // Run a TLS step until it stops asking to wait. Returns its final result;
    // MBEDTLS_ERR_SSL_TIMEOUT when the deadline passed or the server is stopping.
    template <typename F>
    int io(F fn, int timeout_ms) {
        const uint64_t deadline = mono_ms() + static_cast<uint64_t>(timeout_ms);
        for (;;) {
            int r;
            {
                std::lock_guard<std::mutex> lk(g_tls);
                r = fn();
            }
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return r;
            if (stop->load() || mono_ms() >= deadline) return MBEDTLS_ERR_SSL_TIMEOUT;
            wait_fd(net.fd, r == MBEDTLS_ERR_SSL_WANT_WRITE, 100);
        }
    }

    bool handshake() {
        return io([&] { return mbedtls_ssl_handshake(&ssl); }, kHttpTimeoutMs) == 0;
    }

    // Read whatever is available (up to n). >0 bytes, 0 = timeout, <0 = closed/error.
    int read_some(uint8_t* buf, size_t n, int timeout_ms) {
        const int r = io([&] { return mbedtls_ssl_read(&ssl, buf, n); }, timeout_ms);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
        return -1;  // includes peer close-notify (0) and errors
    }

    bool write_all(const uint8_t* p, size_t n) {
        while (n > 0) {
            const int r = io([&] { return mbedtls_ssl_write(&ssl, p, n); }, kHttpTimeoutMs);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }
    bool write_all(const std::string& s) {
        return write_all(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
};

const char* kSecurityHeaders =
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'self'; connect-src 'self' wss:; "
    "img-src 'self' data:; style-src 'self'; script-src 'self'\r\n";

std::string simple_response(int code, const char* reason, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
      << "Content-Type: text/plain; charset=utf-8\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << kSecurityHeaders << "Connection: close\r\n\r\n"
      << body;
    return o.str();
}

void close_fd(int fd) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

}  // namespace

std::mutex& web_tls_mutex() { return g_tls; }

std::string WebServer::url_for(const std::string& ip, uint16_t port, const std::string& token) {
    return "https://" + ip + ":" + std::to_string(port) + "/#t=" + token;
}

std::vector<bool> qr_encode(const std::string& text, int* size) {
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        std::vector<bool> m(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) m[static_cast<size_t>(y) * n + x] = qr.getModule(x, y);
        if (size) *size = n;
        return m;
    } catch (...) {
        if (size) *size = 0;
        return {};
    }
}

WebServer::WebServer(Receiver* rx, EventLog* log) : rx_(rx), log_(log) {}

WebServer::~WebServer() { stop(); }

void WebServer::set_token(const std::string& t) {
    std::lock_guard<std::mutex> lk(token_mu_);
    token_ = t;
}

bool WebServer::load_or_create_credentials(std::string* err) {
    namespace fs = std::filesystem;
    const fs::path dir = config_dir();
    const fs::path cert_file = dir / "web-cert.pem", key_file = dir / "web-key.pem";
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    cert_pem_ = slurp(cert_file);
    key_pem_ = slurp(key_file);

    std::lock_guard<std::mutex> lk(g_tls);
    if (!cert_pem_.empty() && !key_pem_.empty()) {
        // Validate what's on disk; regenerate if corrupt.
        mbedtls_x509_crt c;
        mbedtls_x509_crt_init(&c);
        const bool ok = mbedtls_x509_crt_parse(&c, reinterpret_cast<const unsigned char*>(cert_pem_.c_str()),
                                               cert_pem_.size() + 1) == 0;
        mbedtls_x509_crt_free(&c);
        if (ok) return true;
    }

    // Generate a self-signed P-256 certificate, valid 20 years, for localhost +
    // this machine's LAN addresses. Kept on disk so the browser's "not private"
    // warning only has to be accepted once per phone.
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    int rc = rng_seed(&drbg, &ent);
    if (rc == 0) rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0)
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg);

    std::vector<std::array<unsigned char, 4>> ips;
    for (const auto& a : net::local_addresses()) {
        net::Endpoint e;
        if (net::Endpoint::parse(a.ip, 0, &e)) {
            std::array<unsigned char, 4> b;
            std::memcpy(b.data(), &e.addr, 4);
            ips.push_back(b);
        }
    }
    std::vector<mbedtls_x509_san_list> san(1 + ips.size());
    static const char kLocal[] = "localhost";
    std::memset(san.data(), 0, san.size() * sizeof(mbedtls_x509_san_list));
    san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[0].node.san.unstructured_name.p = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(kLocal));
    san[0].node.san.unstructured_name.len = sizeof kLocal - 1;
    for (size_t i = 0; i < ips.size(); ++i) {
        san[i + 1].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san[i + 1].node.san.unstructured_name.p = ips[i].data();
        san[i + 1].node.san.unstructured_name.len = 4;
    }
    for (size_t i = 0; i + 1 < san.size(); ++i) san[i].next = &san[i + 1];

    unsigned char serial[16];
    if (rc == 0) rc = mbedtls_ctr_drbg_random(&drbg, serial, sizeof serial);
    serial[0] = static_cast<unsigned char>((serial[0] & 0x7F) | 0x01);  // positive, non-zero

    if (rc == 0) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=PhoneMic,O=PhoneMic");
    }
    if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=PhoneMic,O=PhoneMic");
    if (rc == 0) rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial);
    if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20450101000000");
    if (rc == 0) rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (rc == 0) rc = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (rc == 0) rc = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san.data());

    std::vector<unsigned char> cbuf(8192, 0), kbuf(4096, 0);
    if (rc == 0) rc = mbedtls_x509write_crt_pem(&crt, cbuf.data(), cbuf.size(), mbedtls_ctr_drbg_random, &drbg);
    if (rc == 0) rc = mbedtls_pk_write_key_pem(&key, kbuf.data(), kbuf.size());

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    if (rc != 0) {
        if (err) *err = "certificate generation failed: " + mbed_err(rc);
        return false;
    }
    cert_pem_ = reinterpret_cast<char*>(cbuf.data());
    key_pem_ = reinterpret_cast<char*>(kbuf.data());
    crypto_wipe(kbuf.data(), kbuf.size());

    std::ofstream(cert_file, std::ios::binary | std::ios::trunc) << cert_pem_;
    {
        std::ofstream kf(key_file, std::ios::binary | std::ios::trunc);
#ifndef _WIN32
        std::filesystem::permissions(key_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
        kf << key_pem_;
    }
    if (log_) log_->add("Created a TLS certificate for the browser mic");
    return true;
}

bool WebServer::start(uint16_t port, const std::string& token, std::string* err) {
    stop();
    {
        std::lock_guard<std::mutex> lk(g_tls);
        static const bool psa_ok = psa_crypto_init() == PSA_SUCCESS;
        if (!psa_ok) {
            if (err) *err = "crypto init failed";
            return false;
        }
    }
    if (!load_or_create_credentials(err)) return false;
    set_token(token);

    mbedtls_net_context listen;
    mbedtls_net_init(&listen);
    const std::string port_s = std::to_string(port);
    int rc;
    {
        std::lock_guard<std::mutex> lk(g_tls);
        // Bind IPv4 explicitly. With a NULL address mbedTLS binds the first
        // getaddrinfo() result, which on Windows is IPv6 "::" — and Windows
        // IPv6 sockets are IPv6-only, so IPv4 phones could not connect.
        rc = mbedtls_net_bind(&listen, "0.0.0.0", port_s.c_str(), MBEDTLS_NET_PROTO_TCP);
    }
    if (rc != 0) {
        if (err) *err = "can't listen on TCP port " + port_s + " (" + mbed_err(rc) + ")";
        return false;
    }
    listen_fd_ = listen.fd;
    port_ = port;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { accept_loop(); });
    if (log_) log_->add("Browser mic server listening on HTTPS port " + port_s);
    return true;
}

void WebServer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) {
        close_fd(listen_fd_);
        listen_fd_ = -1;
    }
    // Connection threads notice stop_ within ~100 ms.
    for (int i = 0; i < 50 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running_.store(false);
}

void WebServer::accept_loop() {
    while (!stop_.load()) {
        if (!wait_fd(listen_fd_, false, 200)) continue;
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        const auto fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &len);
#ifdef _WIN32
        if (fd == INVALID_SOCKET) continue;
#else
        if (fd < 0) continue;
#endif
        std::string ip = "?";
        if (ss.ss_family == AF_INET) {
            net::Endpoint e;
            e.addr = reinterpret_cast<sockaddr_in*>(&ss)->sin_addr.s_addr;
            ip = e.ip_string();
        }
        if (active_.load() >= kMaxConnections) {
            close_fd(static_cast<int>(fd));
            continue;
        }
        active_.fetch_add(1);
        std::thread([this, fd, ip] {
            serve(static_cast<int>(fd), ip);
            active_.fetch_sub(1);
        }).detach();
    }
}

void WebServer::serve(int fd, std::string peer_ip) {
    Tls tls(fd, &stop_);
    if (!tls.setup(cert_pem_, key_pem_) || !tls.handshake()) return;  // e.g. cert not yet accepted

    // ---- HTTP request ----
    std::string req_buf;
    uint8_t buf[4096];
    ws::HttpRequest req;
    size_t hdr_end = 0;
    const uint64_t deadline = mono_ms() + kHttpTimeoutMs;
    while (!ws::parse_request(req_buf, &req, &hdr_end)) {
        if (req_buf.size() > kMaxHeader || mono_ms() > deadline) return;
        const int n = tls.read_some(buf, sizeof buf, 1000);
        if (n < 0) return;
        req_buf.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
        if (req_buf.find("\r\n\r\n") != std::string::npos && !ws::parse_request(req_buf, &req, &hdr_end)) {
            tls.write_all(simple_response(400, "Bad Request", "bad request\n"));
            return;
        }
    }

    if (req.method != "GET") {
        tls.write_all(simple_response(405, "Method Not Allowed", "GET only\n"));
        return;
    }

    if (req.path != "/ws") {
        const auto* a = find_asset(req.path);
        if (!a) {
            tls.write_all(simple_response(404, "Not Found", "not found\n"));
            return;
        }
        std::ostringstream h;
        h << "HTTP/1.1 200 OK\r\nContent-Type: " << a->mime << "\r\nContent-Length: " << a->size
          << "\r\n" << kSecurityHeaders << "Permissions-Policy: microphone=(self)\r\nConnection: close\r\n\r\n";
        if (tls.write_all(h.str())) tls.write_all(a->data, a->size);
        return;
    }

    // ---- WebSocket upgrade: requires the pairing code ----
    std::string token;
    {
        std::lock_guard<std::mutex> lk(token_mu_);
        token = token_;
    }
    if (!req.is_websocket_upgrade()) {
        tls.write_all(simple_response(400, "Bad Request", "expected a WebSocket upgrade\n"));
        return;
    }
    // Cross-site WebSocket hijacking guard: the page must come from us.
    const std::string origin = req.header("origin");
    if (!origin.empty() && origin != "https://" + req.header("host")) {
        tls.write_all(simple_response(403, "Forbidden", "bad origin\n"));
        return;
    }
    if (token.empty() || !ct_equal(req.query_param("t"), token)) {
        if (log_) log_->add("Browser " + peer_ip + " tried to connect with a wrong or missing code");
        tls.write_all(simple_response(403, "Forbidden", "wrong pairing code\n"));
        return;
    }
    std::ostringstream h;
    h << "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << ws::accept_key(req.header("sec-websocket-key")) << "\r\n\r\n";
    if (!tls.write_all(h.str())) return;

    const uint64_t conn_id = next_conn_id_.fetch_add(1);
    SourceId src;
    src.via = SourceId::Via::Web;
    src.conn_id = conn_id;
    net::Endpoint::parse(peer_ip, 0, &src.ep);

    ws::FrameParser parser(PM_MAX_DATAGRAM);
    // Bytes that arrived right after the upgrade request belong to the stream.
    if (req_buf.size() > hdr_end)
        parser.feed(reinterpret_cast<const uint8_t*>(req_buf.data()) + hdr_end, req_buf.size() - hdr_end);

    auto send_text = [&](const std::string& s) {
        const auto f = ws::encode_frame(ws::kText, reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return tls.write_all(f.data(), f.size());
    };
    int last_state = -1;
    uint64_t last_report = 0, last_rx = mono_ms();
    for (;;) {
        ws::Frame fr;
        bool got = false;
        while (parser.next(&fr)) {
            got = true;
            last_rx = mono_ms();
            if (fr.opcode == ws::kBinary && fr.fin) {
                const uint8_t st = rx_->submit(src, fr.payload.data(), fr.payload.size(), /*trusted=*/true);
                const uint64_t now = mono_ms();
                if (st != last_state || now - last_report >= 1000) {
                    const auto snap = rx_->snapshot();
                    send_text("{\"state\":" + std::to_string(st) + ",\"buffer\":" +
                              std::to_string(snap.mic_buffer_ms) + ",\"lost\":" + std::to_string(snap.lost) + "}");
                    last_state = st;
                    last_report = now;
                }
            } else if (fr.opcode == ws::kPing) {
                const auto pong = ws::encode_frame(ws::kPong, fr.payload.data(), fr.payload.size());
                tls.write_all(pong.data(), pong.size());
            } else if (fr.opcode == ws::kClose) {
                const auto c = ws::encode_frame(ws::kClose, fr.payload.data(), std::min<size_t>(fr.payload.size(), 2));
                tls.write_all(c.data(), c.size());
                rx_->release_conn(conn_id);
                return;
            } else if (fr.opcode == ws::kText || fr.opcode == ws::kPong) {
                // ignored
            } else {
                break;  // fragmented/unknown: not something our client sends
            }
        }
        if (parser.error()) break;
        if (!got && mono_ms() - last_rx > static_cast<uint64_t>(kWsIdleTimeoutMs)) break;
        const int n = tls.read_some(buf, sizeof buf, 200);
        if (n < 0) break;
        if (n > 0) parser.feed(buf, static_cast<size_t>(n));
        if (stop_.load()) break;
    }
    rx_->release_conn(conn_id);
}

}  // namespace phonemic
