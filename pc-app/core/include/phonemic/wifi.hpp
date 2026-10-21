// Wi-Fi transport: the phone streams to the PC over UDP on the local network.
//
//   phone app ─► HELLO (broadcast) ─► WifiServer ─► ANNOUNCE (name, port, PIN?)
//   phone app ─► AUDIO datagrams  ─► WifiServer ─► Receiver ─► STATUS back
//
// One packet per datagram, no framing. A PIN set on the PC (Receiver::
// set_passphrase) means the audio must be sealed with it, so nobody else on
// the network can talk into the microphone. Discovery replies are rate-limited
// and only ever go to the address that asked.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phonemic/net.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/util.hpp"

namespace phonemic {

class WifiServer {
public:
    WifiServer(Receiver* rx, EventLog* log) : rx_(rx), log_(log) {}
    ~WifiServer();
    WifiServer(const WifiServer&) = delete;
    WifiServer& operator=(const WifiServer&) = delete;

    // Bind UDP 0.0.0.0:port (0 = any free port).
    bool start(uint16_t port, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    // The name phones list in discovery (default: this PC's host name).
    void set_host_name(const std::string& n);

private:
    void loop();
    void handle(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now);
    void answer_hello(const uint8_t* data, size_t len, const net::Endpoint& from, uint64_t now);
    void send_status(const net::Endpoint& to, const SourceId& src, uint8_t state);

    Receiver* rx_;
    EventLog* log_;
    net::UdpSocket sock_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false};
    std::mutex name_mu_;
    std::string host_name_;
    // Loop thread only.
    std::map<std::pair<uint32_t, uint16_t>, uint64_t> conn_ids_;  // address -> connection id
    std::vector<uint64_t> hello_times_;
    uint64_t last_ok_status_ms_ = 0, last_reject_reply_ms_ = 0;
};

}  // namespace phonemic
