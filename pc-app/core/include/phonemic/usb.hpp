// USB transport: a phone on a cable streams over TCP through `adb reverse`.
//
//   phone app ─► TCP 127.0.0.1:4010 (on the phone) ─► adb ─► TCP 127.0.0.1:port (PC)
//
// The same [u16 LE length][packet] frames as Bluetooth (bluetooth.hpp), pumped
// by the same serve_stream(). The listener binds 127.0.0.1 only: it is
// reachable through adb, never from the network. AdbBridge finds adb and keeps
// `adb reverse` set up for every plugged-in phone, so the user only enables USB
// debugging once. adb is the Android SDK tool, not a driver.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phonemic/bluetooth.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/util.hpp"

namespace phonemic {

class UsbServer {
public:
    UsbServer(Receiver* rx, EventLog* log) : rx_(rx), log_(log) {}
    ~UsbServer();
    UsbServer(const UsbServer&) = delete;
    UsbServer& operator=(const UsbServer&) = delete;

    // Listen on 127.0.0.1:port (0 = any free port).
    bool start(uint16_t port, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    int connections() const { return active_.load(); }

private:
    void accept_loop();

    Receiver* rx_;
    EventLog* log_;
    intptr_t listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<int> active_{0};
    std::mutex mu_;
    std::vector<ByteStream*> streams_;  // open connections, closed by stop()
};

class AdbBridge {
public:
    enum class State { Starting, NoAdb, NoDevice, Unauthorized, Ready };

    explicit AdbBridge(EventLog* log) : log_(log) {}
    ~AdbBridge();
    // Map the phone's 127.0.0.1:PM_DEFAULT_PORT to this PC's `port`.
    void start(uint16_t port);
    void stop();
    State state() const { return state_.load(); }
    std::string device() const;  // model/serial of the connected phone
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

}  // namespace phonemic
