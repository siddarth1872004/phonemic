// USB mode: a phone on a cable streams over TCP through `adb reverse`.
//
//   phone app → TCP 127.0.0.1:4010 (on the phone) → adb → TCP 127.0.0.1:4010 (PC)
//
// `adb reverse` only forwards TCP, so packets travel as length-prefixed frames:
// [u16 little-endian length][one PhoneMic datagram], the same AUDIO packets as
// over UDP. The PC answers with framed STATUS packets on the same connection.
// The listener binds 127.0.0.1 only: it is reachable through adb, never from
// the network. AdbBridge finds adb and keeps `adb reverse` set up for every
// plugged-in phone, so the user only has to enable USB debugging once.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phonemic/receiver.hpp"
#include "phonemic/util.hpp"

namespace phonemic {

class UsbIngest {
public:
    UsbIngest(Receiver* rx, EventLog* log) : rx_(rx), log_(log) {}
    ~UsbIngest();
    bool start(uint16_t port, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    int connections() const { return active_.load(); }

private:
    void accept_loop();
    void serve(intptr_t fd);

    Receiver* rx_;
    EventLog* log_;
    intptr_t listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<int> active_{0};
    std::atomic<uint64_t> next_id_{1};
};

class AdbBridge {
public:
    enum class State { Starting, NoAdb, NoDevice, Unauthorized, Ready };

    explicit AdbBridge(EventLog* log) : log_(log) {}
    ~AdbBridge();
    void start(uint16_t port);
    void stop();
    State state() const { return state_.load(); }
    std::string device() const;     // model/serial of the connected phone
    std::string adb_path() const;

private:
    void loop(uint16_t port);
    std::string find_adb();

    EventLog* log_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Starting};
    mutable std::mutex mu_;
    std::string device_, adb_;
};

// Exposed for tests: split a TCP byte stream into length-prefixed frames.
class FrameSplitter {
public:
    explicit FrameSplitter(size_t max_frame) : max_(max_frame) {}
    void feed(const uint8_t* d, size_t n) { buf_.insert(buf_.end(), d, d + n); }
    // false = need more bytes; error() = a frame was empty or too large.
    bool next(std::vector<uint8_t>* out);
    bool error() const { return error_; }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    size_t max_;
    bool error_ = false;
};

}  // namespace phonemic
