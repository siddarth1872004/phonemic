#include "phonemic/net.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET  // missing from some MinGW headers
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace phonemic::net {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kInvalid = INVALID_SOCKET;
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WsaInit() { WSACleanup(); }
};
void ensure_init() { static WsaInit init; }
std::string last_error_string() {
    const int e = WSAGetLastError();
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf,
                   sizeof buf, nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s + " (" + std::to_string(e) + ")";
}
#else
using sock_t = int;
constexpr sock_t kInvalid = -1;
void ensure_init() {}
std::string last_error_string() { return std::strerror(errno); }
#endif

bool is_link_local(uint32_t host) { return (host & 0xFFFF0000u) == 0xA9FE0000u; }
bool is_loopback(uint32_t host) { return (host >> 24) == 127; }

std::string primary_ip() {
    // Ask the OS which interface it would use toward a public address. No packet
    // is sent (UDP connect only sets the default destination).
    ensure_init();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalid) return {};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof local;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf);
            out = buf;
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    return out == "0.0.0.0" ? std::string() : out;
}

}  // namespace

void init() { ensure_init(); }

std::string Endpoint::ip_string() const {
    in_addr a{};
    a.s_addr = addr;
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

std::string Endpoint::to_string() const { return ip_string() + ":" + std::to_string(port); }

bool Endpoint::parse(const std::string& ip, uint16_t port, Endpoint* out) {
    ensure_init();
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    out->addr = a.s_addr;
    out->port = port;
    return true;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::is_open() const { return fd_ != static_cast<decltype(fd_)>(kInvalid); }

void UdpSocket::close() {
    if (!is_open()) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd_));
#else
    ::close(fd_);
#endif
    fd_ = static_cast<decltype(fd_)>(kInvalid);
}

bool UdpSocket::open(uint16_t port, std::string* err) {
    ensure_init();
    close();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        if (err) *err = "socket(): " + last_error_string();
        return false;
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" (e.g. a STATUS reply to a phone
    // that just stopped) makes the next recvfrom() fail with WSAECONNRESET.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    // Refuse to share the port with another process (a second PhoneMic).
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#endif
    // A bigger receive buffer absorbs Wi-Fi bursts without kernel drops.
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        if (err) *err = "bind(UDP " + std::to_string(port) + "): " + last_error_string();
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }
    fd_ = static_cast<decltype(fd_)>(s);
    return true;
}

uint16_t UdpSocket::local_port() const {
    if (!is_open()) return 0;
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (::getsockname(static_cast<sock_t>(fd_), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

bool UdpSocket::set_broadcast(bool on) {
    int v = on ? 1 : 0;
    return setsockopt(static_cast<sock_t>(fd_), SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&v), sizeof v) == 0;
}

int UdpSocket::recv(uint8_t* buf, size_t cap, Endpoint* from, int timeout_ms) {
    if (!is_open()) return -1;
    const sock_t s = static_cast<sock_t>(fd_);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int r = ::select(static_cast<int>(s + 1), &set, nullptr, nullptr, &tv);
    if (r == 0) return 0;
    if (r < 0) {
#ifndef _WIN32
        if (errno == EINTR) return 0;
#endif
        return -1;
    }
    sockaddr_in a{};
    socklen_t len = sizeof a;
    const auto n = ::recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                              reinterpret_cast<sockaddr*>(&a), &len);
    if (n < 0) {
#ifdef _WIN32
        const int e = WSAGetLastError();
        // Oversized datagram: it was truncated; report it so the caller drops it.
        if (e == WSAEMSGSIZE) return static_cast<int>(cap);
        if (e == WSAECONNRESET) return 0;
#endif
        return -1;
    }
    if (from) {
        from->addr = a.sin_addr.s_addr;
        from->port = ntohs(a.sin_port);
    }
    return static_cast<int>(n);
}

bool UdpSocket::send_to(const Endpoint& to, const uint8_t* data, size_t n) {
    if (!is_open()) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = to.addr;
    a.sin_port = htons(to.port);
    const auto sent = ::sendto(static_cast<sock_t>(fd_), reinterpret_cast<const char*>(data),
                               static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return sent >= 0 && static_cast<size_t>(sent) == n;
}

std::vector<LocalAddress> local_addresses() {
    ensure_init();
    std::vector<LocalAddress> out;
    auto add = [&](uint32_t addr_be, const std::string& name) {
        const uint32_t host = ntohl(addr_be);
        if (is_loopback(host) || is_link_local(host) || host == 0) return;
        Endpoint e;
        e.addr = addr_be;
        const std::string ip = e.ip_string();
        for (const auto& x : out)
            if (x.ip == ip) return;
        out.push_back({ip, name, false});
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp) continue;
            char name[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, ad->FriendlyName, -1, name, sizeof name - 1, nullptr, nullptr);
            for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                add(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr, name);
            }
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            add(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr, i->ifa_name ? i->ifa_name : "");
        }
        freeifaddrs(ifs);
    }
#endif
    const std::string primary = primary_ip();
    for (auto& a : out) a.primary = (a.ip == primary);
    // Primary first, then private ranges (the ones a phone on Wi-Fi can reach).
    auto rank = [](const LocalAddress& a) {
        if (a.primary) return 0;
        if (a.ip.rfind("192.168.", 0) == 0 || a.ip.rfind("10.", 0) == 0) return 1;
        if (a.ip.rfind("172.", 0) == 0) return 2;
        return 3;
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const LocalAddress& x, const LocalAddress& y) { return rank(x) < rank(y); });
    return out;
}

std::string host_name() {
    ensure_init();
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return "PhoneMic PC";
    std::string s(buf);
    const auto dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    return s.empty() ? std::string("PhoneMic PC") : s;
}

}  // namespace phonemic::net
