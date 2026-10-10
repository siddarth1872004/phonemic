// Built-in "browser mic": an HTTPS + WebSocket server inside the desktop app.
//
// Any phone (Android or iPhone) can open https://<pc-ip>:8443/#t=<code> — or
// scan the QR code in the app — and stream its microphone with no install.
// Audio goes into the very same pipeline as the native app (Voice Focus, voice
// changer, virtual microphone). The old separate Rust gateway only played to
// the speakers and accepted audio from anyone on the network.
//
// Security: TLS (browsers require a secure context for the mic) with a
// self-signed certificate that is generated once and kept, so the browser
// warning only appears the first time; and a random pairing code that must be
// presented to open the audio WebSocket.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phonemic/receiver.hpp"
#include "phonemic/util.hpp"

namespace phonemic {

class WebServer {
public:
    WebServer(Receiver* rx, EventLog* log);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool start(uint16_t port, const std::string& token, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    void set_token(const std::string& t);
    int active_connections() const { return active_.load(); }

    static std::string url_for(const std::string& ip, uint16_t port, const std::string& token);

private:
    void accept_loop();
    void serve(int fd, std::string peer_ip);
    bool load_or_create_credentials(std::string* err);

    Receiver* rx_;
    EventLog* log_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<int> active_{0};
    uint16_t port_ = 0;
    int listen_fd_ = -1;
    std::thread thread_;
    std::mutex token_mu_;
    std::string token_;
    std::string cert_pem_, key_pem_;
    std::atomic<uint64_t> next_conn_id_{1};
};

// mbedTLS is not thread-safe in this configuration: any other in-process TLS
// user (e.g. a test client) must hold this lock around mbedtls_* calls.
std::mutex& web_tls_mutex();

// QR code modules for `text` (row-major, true = dark). Empty on failure.
std::vector<bool> qr_encode(const std::string& text, int* size);

}  // namespace phonemic
