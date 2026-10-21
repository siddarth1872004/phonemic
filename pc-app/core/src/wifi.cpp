#include "phonemic/wifi.hpp"

#include <phonemic/wire.h>

#include <algorithm>
#include <cstring>

namespace phonemic {

namespace {
constexpr int kRecvTimeoutMs = 100;
constexpr uint64_t kStatusIntervalMs = 500;
constexpr uint64_t kRejectReplyMs = 250;
constexpr size_t kMaxHelloPerSecond = 10;
constexpr size_t kMaxSenders = 32;
}  // namespace

WifiServer::~WifiServer() { stop(); }

bool WifiServer::start(uint16_t port, std::string* err) {
    stop();
    net::init();
    if (!sock_.open(port, err)) return false;
    sock_.set_broadcast(true);
    port_ = sock_.local_port();
    {
        std::lock_guard<std::mutex> lk(name_mu_);
        if (host_name_.empty()) host_name_ = net::host_name();
    }
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void WifiServer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    sock_.close();
    running_.store(false);
}

void WifiServer::set_host_name(const std::string& n) {
    std::lock_guard<std::mutex> lk(name_mu_);
    host_name_ = n.empty() ? net::host_name() : n;
}

void WifiServer::loop() {
    std::vector<uint8_t> buf(PM_MAX_DATAGRAM + 64);
    while (!stop_.load()) {
        net::Endpoint from;
        const int n = sock_.recv(buf.data(), buf.size(), &from, kRecvTimeoutMs);
        if (n > 0) handle(buf.data(), static_cast<size_t>(n), from, mono_ms());
    }
}

void WifiServer::handle(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now) {
    uint8_t kind = 0;
    if (pm_peek(data, len, &kind) != PM_OK) return;
    if (kind == PM_KIND_HELLO) {
        answer_hello(data, len, from, now);
        return;
    }
    if (kind != PM_KIND_AUDIO) return;  // ANNOUNCE / STATUS are PC->phone only

    SourceId src;
    src.via = SourceId::Via::Wifi;
    const auto key = std::make_pair(from.addr, from.port);
    auto it = conn_ids_.find(key);
    if (it == conn_ids_.end()) {
        if (conn_ids_.size() >= kMaxSenders) conn_ids_.clear();
        it = conn_ids_.emplace(key, Receiver::new_conn_id()).first;
    }
    src.conn_id = it->second;
    src.name = from.ip_string();
    pm_audio_header h{};
    const uint8_t* payload = nullptr;
    if (pm_decode_audio(data, len, &h, &payload) == PM_OK) src.session = h.session_id;

    const uint8_t st = rx_->submit(src, data, len);
    if (st == PM_STATUS_OK) {
        if (now - last_ok_status_ms_ >= kStatusIntervalMs) {
            last_ok_status_ms_ = now;
            send_status(from, src, st);
        }
    } else if (now - last_reject_reply_ms_ >= kRejectReplyMs) {
        last_reject_reply_ms_ = now;
        send_status(from, src, st);
    }
}

void WifiServer::send_status(const net::Endpoint& to, const SourceId& src, uint8_t state) {
    const pm_status s = rx_->make_status(src, state);
    uint8_t out[PM_STATUS_LEN];
    pm_encode_status(out, &s);
    sock_.send_to(to, out, sizeof out);
}

void WifiServer::answer_hello(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now) {
    uint32_t nonce = 0;
    if (pm_decode_hello(data, len, &nonce) != PM_OK) return;
    hello_times_.erase(std::remove_if(hello_times_.begin(), hello_times_.end(),
                                      [&](uint64_t t) { return now - t >= 1000; }),
                       hello_times_.end());
    if (hello_times_.size() >= kMaxHelloPerSecond) return;
    hello_times_.push_back(now);

    pm_announce a{};
    a.nonce = nonce;
    a.audio_port = port_;
    if (rx_->has_passphrase()) a.flags |= PM_ANNOUNCE_FLAG_PIN;
    if (rx_->busy()) a.flags |= PM_ANNOUNCE_FLAG_BUSY;
    {
        std::lock_guard<std::mutex> lk(name_mu_);
        std::strncpy(a.name, host_name_.c_str(), PM_MAX_NAME);
        a.name[PM_MAX_NAME] = '\0';
    }
    uint8_t out[PM_ANNOUNCE_MIN_LEN + PM_MAX_NAME];
    const int n = pm_encode_announce(out, sizeof out, &a);
    if (n > 0) sock_.send_to(from, out, static_cast<size_t>(n));
}

}  // namespace phonemic
