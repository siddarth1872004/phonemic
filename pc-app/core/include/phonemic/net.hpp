// Minimal cross-platform IPv4 UDP socket + LAN address helpers (Winsock / BSD).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phonemic::net {

struct Endpoint {
    uint32_t addr = 0;  // IPv4, network byte order
    uint16_t port = 0;  // host byte order
    std::string to_string() const;    // "a.b.c.d:port"
    std::string ip_string() const;    // "a.b.c.d"
    bool operator==(const Endpoint& o) const { return addr == o.addr && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    static bool parse(const std::string& ip, uint16_t port, Endpoint* out);
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bind 0.0.0.0:port (0 = ephemeral). Returns false and fills `err` on failure
    // (most commonly: the port is already in use by another PhoneMic).
    bool open(uint16_t port, std::string* err);
    void close();
    bool is_open() const;
    uint16_t local_port() const;

    // Wait up to timeout_ms for one datagram. Returns its size, 0 on timeout,
    // -1 on error.
    int recv(uint8_t* buf, size_t cap, Endpoint* from, int timeout_ms);
    bool send_to(const Endpoint& to, const uint8_t* data, size_t n);
    bool set_broadcast(bool on);

private:
#ifdef _WIN32
    uintptr_t fd_ = ~static_cast<uintptr_t>(0);
#else
    int fd_ = -1;
#endif
};

struct LocalAddress {
    std::string ip;
    std::string interface_name;
    bool primary = false;  // the one the default route uses
};

// LAN IPv4 addresses of this machine, primary first, loopback/link-local skipped.
// Initialise the socket library (Winsock); idempotent, no-op elsewhere.
void init();

std::vector<LocalAddress> local_addresses();
std::string host_name();

}  // namespace phonemic::net
