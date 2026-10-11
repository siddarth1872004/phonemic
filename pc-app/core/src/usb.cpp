#include "phonemic/usb.hpp"

#include <phonemic/wire.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "phonemic/net.hpp"
#include "phonemic/process.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace phonemic {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
void close_sock(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
constexpr intptr_t kBad = static_cast<intptr_t>(INVALID_SOCKET);
#else
using sock_t = int;
void close_sock(intptr_t s) { ::close(static_cast<int>(s)); }
constexpr intptr_t kBad = -1;
#endif

constexpr int kMaxConnections = 2;
constexpr uint64_t kIdleTimeoutMs = 10000;

bool readable(intptr_t fd, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<sock_t>(fd), &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(static_cast<int>(fd + 1), &set, nullptr, nullptr, &tv) > 0;
}

bool send_all(intptr_t fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        const auto r = ::send(static_cast<sock_t>(fd), reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

}  // namespace

// ---- FrameSplitter -----------------------------------------------------------------

bool FrameSplitter::next(std::vector<uint8_t>* out) {
    if (error_) return false;
    if (buf_.size() - pos_ < 2) return false;
    const size_t len = static_cast<size_t>(buf_[pos_]) | (static_cast<size_t>(buf_[pos_ + 1]) << 8);
    if (len == 0 || len > max_) {
        error_ = true;
        return false;
    }
    if (buf_.size() - pos_ < 2 + len) return false;
    out->assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 2),
                buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 2 + len));
    pos_ += 2 + len;
    if (pos_ > 4096) {  // compact occasionally
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return true;
}

// ---- UsbIngest -----------------------------------------------------------------------

UsbIngest::~UsbIngest() { stop(); }

bool UsbIngest::start(uint16_t port, std::string* err) {
    stop();
    net::init();
    const sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<intptr_t>(s) == kBad) {
        if (err) *err = "socket() failed";
        return false;
    }
#ifdef _WIN32
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#else
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  // quick restart after close
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // reachable via adb only, not the LAN
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0) {
        if (err) *err = "can't listen on TCP 127.0.0.1:" + std::to_string(port);
        close_sock(static_cast<intptr_t>(s));
        return false;
    }
    listen_fd_ = static_cast<intptr_t>(s);
    socklen_t len = sizeof a;
    port_ = ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0 ? ntohs(a.sin_port) : port;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void UsbIngest::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ != kBad) {
        close_sock(listen_fd_);
        listen_fd_ = kBad;
    }
    for (int i = 0; i < 50 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running_.store(false);
}

void UsbIngest::accept_loop() {
    while (!stop_.load()) {
        if (!readable(listen_fd_, 200)) continue;
        const sock_t c = ::accept(static_cast<sock_t>(listen_fd_), nullptr, nullptr);
        if (static_cast<intptr_t>(c) == kBad) continue;
        if (active_.load() >= kMaxConnections) {
            close_sock(static_cast<intptr_t>(c));
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        active_.fetch_add(1);
        std::thread([this, c] {
            serve(static_cast<intptr_t>(c));
            active_.fetch_sub(1);
        }).detach();
    }
}

void UsbIngest::serve(intptr_t fd) {
    const uint64_t conn = next_id_.fetch_add(1);
    SourceId src;
    src.via = SourceId::Via::Usb;
    src.conn_id = conn;
    net::Endpoint::parse("127.0.0.1", 0, &src.ep);

    FrameSplitter frames(PM_MAX_DATAGRAM);
    std::vector<uint8_t> frame;
    uint8_t buf[8192];
    uint64_t last_rx = mono_ms(), last_status = 0;
    int last_state = -1;
    uint32_t last_session = 0;

    while (!stop_.load()) {
        if (readable(fd, 200)) {
            const auto n = ::recv(static_cast<sock_t>(fd), reinterpret_cast<char*>(buf), sizeof buf, 0);
            if (n <= 0) break;  // the phone (or adb) closed the connection
            frames.feed(buf, static_cast<size_t>(n));
            last_rx = mono_ms();
        } else if (mono_ms() - last_rx > kIdleTimeoutMs) {
            break;
        }
        while (frames.next(&frame)) {
            uint8_t version = 0, kind = 0;
            if (pm_peek(frame.data(), frame.size(), &version, &kind) != PM_OK || kind != PM_KIND_AUDIO) continue;
            pm_audio_header h{};
            const uint8_t* payload = nullptr;
            if (pm_decode_audio(frame.data(), frame.size(), &h, &payload) == PM_OK) last_session = h.session_id;
            // Same auth policy as Wi-Fi: with a PIN set, audio must be encrypted.
            const uint8_t st = rx_->submit(src, frame.data(), frame.size(), /*trusted=*/false);
            const uint64_t now = mono_ms();
            if (st != last_state || now - last_status >= 500) {
                const auto snap = rx_->snapshot();
                pm_status s{};
                s.state = st;
                s.session_id = last_session;
                s.buffer_ms = static_cast<uint16_t>(snap.mic_buffer_ms > 65535 ? 65535 : snap.mic_buffer_ms);
                s.received = snap.peer_is_usb ? snap.session_received : 0;
                s.lost = snap.peer_is_usb ? snap.session_lost : 0;
                uint8_t out[2 + PM_STATUS_LEN] = {static_cast<uint8_t>(PM_STATUS_LEN), 0};
                pm_encode_status(out + 2, &s);
                if (!send_all(fd, out, sizeof out)) break;
                last_state = st;
                last_status = now;
            }
        }
        if (frames.error()) break;  // not a PhoneMic stream
    }
    close_sock(fd);
    rx_->release_conn(conn);
}

// ---- AdbBridge -------------------------------------------------------------------------

AdbBridge::~AdbBridge() { stop(); }

void AdbBridge::start(uint16_t port) {
    stop();
    stop_.store(false);
    thread_ = std::thread([this, port] { loop(port); });
}

void AdbBridge::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

std::string AdbBridge::device() const {
    std::lock_guard<std::mutex> lk(mu_);
    return device_;
}

std::string AdbBridge::adb_path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return adb_;
}

std::string AdbBridge::find_adb() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const char* exe = "adb.exe";
#else
    const char* exe = "adb";
#endif
    std::vector<std::string> candidates = {"adb"};
    for (const char* env : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (const char* v = std::getenv(env); v && *v) candidates.push_back((fs::path(v) / "platform-tools" / exe).string());
#if defined(_WIN32)
    if (const char* v = std::getenv("LOCALAPPDATA"))
        candidates.push_back((fs::path(v) / "Android" / "Sdk" / "platform-tools" / exe).string());
#elif defined(__APPLE__)
    if (const char* v = std::getenv("HOME"))
        candidates.push_back((fs::path(v) / "Library/Android/sdk/platform-tools" / exe).string());
#else
    if (const char* v = std::getenv("HOME")) candidates.push_back((fs::path(v) / "Android/Sdk/platform-tools" / exe).string());
#endif
    for (const auto& c : candidates) {
        const auto r = run_process({c, "version"}, 5000);
        if (r.started && r.exit_code == 0) return c;
    }
    return {};
}

void AdbBridge::loop(uint16_t port) {
    std::string adb;
    uint64_t last_find = 0;
    std::string reversed_serial;
    // The phone app always connects to its own 127.0.0.1:4010; map that to
    // whatever port this PC listens on.
    const std::string phone_spec = "tcp:" + std::to_string(PM_DEFAULT_PORT);
    const std::string spec = "tcp:" + std::to_string(port);
    while (!stop_.load()) {
        const uint64_t now = mono_ms();
        if (adb.empty() && (last_find == 0 || now - last_find > 30000)) {
            last_find = now;
            adb = find_adb();
            std::lock_guard<std::mutex> lk(mu_);
            adb_ = adb;
        }
        if (adb.empty()) {
            state_.store(State::NoAdb);
        } else {
            const auto devs = run_process({adb, "devices", "-l"}, 5000);
            std::string serial, model;
            bool unauthorized = false;
            std::istringstream in(devs.output);
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                std::string s, st;
                ls >> s >> st;
                if (st == "unauthorized") unauthorized = true;
                if (st != "device" || !serial.empty()) continue;
                serial = s;
                std::string tok;
                while (ls >> tok)
                    if (tok.rfind("model:", 0) == 0) model = tok.substr(6);
            }
            if (serial.empty()) {
                state_.store(unauthorized ? State::Unauthorized : State::NoDevice);
                reversed_serial.clear();
            } else {
                // Idempotent: re-applied every few seconds so a replugged or
                // restarted phone gets it again.
                const auto r = run_process({adb, "-s", serial, "reverse", phone_spec, spec}, 5000);
                if (r.started && r.exit_code == 0) {
                    state_.store(State::Ready);
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        device_ = model.empty() ? serial : model;
                    }
                    if (serial != reversed_serial && log_)
                        log_->add("USB: phone " + (model.empty() ? serial : model) + " ready (adb reverse " + spec + ")");
                    reversed_serial = serial;
                } else {
                    state_.store(State::NoDevice);
                }
            }
        }
        for (int i = 0; i < 30 && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace phonemic
