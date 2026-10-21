#include "phonemic/bluetooth.hpp"

#include <phonemic/wire.h>

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace phonemic {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
void close_sock(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
constexpr int kShutBoth = SD_BOTH;
#else
using sock_t = int;
void close_sock(intptr_t s) { ::close(static_cast<int>(s)); }
constexpr int kShutBoth = SHUT_RDWR;
#endif

constexpr int kMaxConnections = 2;
constexpr uint64_t kIdleTimeoutMs = 10000;
constexpr uint64_t kStatusIntervalMs = 500;

class SocketStream : public ByteStream {
public:
    explicit SocketStream(intptr_t fd) : fd_(fd) {}
    ~SocketStream() override { close_sock(fd_); }

    int read(uint8_t* buf, size_t cap, int timeout_ms) override {
        if (closed_.load()) return -1;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(static_cast<sock_t>(fd_), &set);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int r = ::select(static_cast<int>(fd_ + 1), &set, nullptr, nullptr, &tv);
        if (r < 0) return -1;
        if (r == 0) return closed_.load() ? -1 : 0;
        const auto n = ::recv(static_cast<sock_t>(fd_), reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
        return n > 0 ? static_cast<int>(n) : -1;
    }

    bool write(const uint8_t* p, size_t n) override {
        while (n > 0) {
            const auto r = ::send(static_cast<sock_t>(fd_), reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

    void close() override {
        closed_.store(true);
        ::shutdown(static_cast<sock_t>(fd_), kShutBoth);  // wakes a blocked select/recv
    }

private:
    intptr_t fd_;
    std::atomic<bool> closed_{false};
};

}  // namespace

std::unique_ptr<ByteStream> socket_stream(intptr_t fd) { return std::make_unique<SocketStream>(fd); }

// ---- FrameSplitter -----------------------------------------------------------------

bool FrameSplitter::next(std::vector<uint8_t>* out) {
    if (error_) return false;
    if (buf_.size() - pos_ < PM_FRAME_PREFIX_LEN) return false;
    const size_t len = static_cast<size_t>(buf_[pos_]) | (static_cast<size_t>(buf_[pos_ + 1]) << 8);
    if (len == 0 || len > max_) {
        error_ = true;
        return false;
    }
    if (buf_.size() - pos_ < PM_FRAME_PREFIX_LEN + len) return false;
    out->assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + PM_FRAME_PREFIX_LEN),
                buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + PM_FRAME_PREFIX_LEN + len));
    pos_ += PM_FRAME_PREFIX_LEN + len;
    if (pos_ > 4096) {  // compact occasionally
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return true;
}

// ---- serve_stream --------------------------------------------------------------------

void serve_stream(Receiver* rx, ByteStream& s, const SourceId& src_in, const std::atomic<bool>& stop) {
    SourceId src = src_in;
    FrameSplitter frames(PM_AUDIO_HEADER_LEN + PM_MAX_PAYLOAD);
    std::vector<uint8_t> frame;
    uint8_t buf[4096];
    uint64_t last_rx = mono_ms(), last_status = 0;
    int last_state = -1;

    while (!stop.load()) {
        const int n = s.read(buf, sizeof buf, 200);
        if (n < 0) break;  // the phone disconnected
        if (n > 0) {
            frames.feed(buf, static_cast<size_t>(n));
            last_rx = mono_ms();
        } else if (mono_ms() - last_rx > kIdleTimeoutMs) {
            break;
        }
        while (frames.next(&frame)) {
            uint8_t kind = 0;
            if (pm_peek(frame.data(), frame.size(), &kind) != PM_OK || kind != PM_KIND_AUDIO) continue;
            pm_audio_header h{};
            const uint8_t* payload = nullptr;
            if (pm_decode_audio(frame.data(), frame.size(), &h, &payload) == PM_OK) src.session = h.session_id;
            const uint8_t st = rx->submit(src, frame.data(), frame.size());
            const uint64_t now = mono_ms();
            if (st != last_state || now - last_status >= kStatusIntervalMs) {
                const pm_status status = rx->make_status(src, st);
                uint8_t out[PM_FRAME_PREFIX_LEN + PM_STATUS_LEN];
                pm_write_frame_prefix(out, PM_STATUS_LEN);
                pm_encode_status(out + PM_FRAME_PREFIX_LEN, &status);
                if (!s.write(out, sizeof out)) return;
                last_state = st;
                last_status = now;
            }
        }
        if (frames.error()) break;  // not a PhoneMic stream
    }
}

// ---- BluetoothServer -----------------------------------------------------------------

BluetoothServer::~BluetoothServer() { stop(); }

void BluetoothServer::start() {
    stop();
    stop_.store(false);
    set_state(State::Starting);
    thread_ = std::thread([this] { platform_loop(); });
}

void BluetoothServer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto* s : streams_) s->close();
    }
    for (int i = 0; i < 60 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

std::string BluetoothServer::adapter_name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return adapter_name_;
}

std::string BluetoothServer::detail() const {
    std::lock_guard<std::mutex> lk(mu_);
    return detail_;
}

void BluetoothServer::set_state(State s, const std::string& detail) {
    const State old = state_.exchange(s);
    std::string old_detail;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_detail = detail_;
        detail_ = detail;
    }
    if (!log_ || (old == s && old_detail == detail)) return;
    switch (s) {
        case State::Ready: log_->add("Bluetooth ready: pick \"" + adapter_name() + "\" in the phone app"); break;
        case State::PoweredOff: log_->add("Bluetooth is switched off on this PC"); break;
        case State::Unavailable:
        case State::Failed: if (!detail.empty()) log_->add("Bluetooth: " + detail); break;
        case State::Starting: break;
    }
}

void BluetoothServer::set_adapter_name(const std::string& n) {
    std::lock_guard<std::mutex> lk(mu_);
    adapter_name_ = n;
}

bool BluetoothServer::sleep_unless_stopped(int ms) {
    for (int t = 0; t < ms && !stop_.load(); t += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !stop_.load();
}

void BluetoothServer::accept(std::unique_ptr<ByteStream> s, const std::string& peer_name) {
    if (stop_.load() || active_.load() >= kMaxConnections) return;  // `s` closes on return
    active_.fetch_add(1);
    SourceId src;
    src.conn_id = next_id_.fetch_add(1);
    src.name = peer_name.empty() ? "Phone" : peer_name;
    if (log_) log_->add(src.name + " connected over Bluetooth");
    ByteStream* raw = s.get();
    {
        std::lock_guard<std::mutex> lk(mu_);
        streams_.push_back(raw);
    }
    std::thread([this, src, st = std::move(s)]() mutable {
        serve_stream(rx_, *st, src, stop_);
        {
            std::lock_guard<std::mutex> lk(mu_);
            streams_.erase(std::remove(streams_.begin(), streams_.end(), st.get()), streams_.end());
        }
        st.reset();
        rx_->release_conn(src.conn_id);
        active_.fetch_sub(1);
    }).detach();
}

}  // namespace phonemic
