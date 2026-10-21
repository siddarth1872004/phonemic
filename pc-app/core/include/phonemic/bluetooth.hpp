// Bluetooth transport: the phone streams to the PC over an RFCOMM connection.
//
//   phone app ─► RFCOMM (service PM_SERVICE_UUID) ─► BluetoothServer ─► Receiver
//
// Only the operating system's own Bluetooth stack is used (Winsock AF_BTH on
// Windows, BlueZ over D-Bus on Linux), so nothing extra is installed. The PC publishes the PhoneMic service (SDP record); the phone finds
// it by UUID on the paired PC. Pairing authenticates the phone and the radio
// encrypts the link.
//
// RFCOMM is a byte stream: packets travel as [u16 LE length][packet] frames,
// and the PC answers with framed STATUS packets on the same connection.
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

// A connected byte stream (an RFCOMM channel, or a socket pair in tests).
class ByteStream {
public:
    virtual ~ByteStream() = default;
    // > 0 bytes read, 0 = nothing within timeout_ms, < 0 = closed or failed.
    virtual int read(uint8_t* buf, size_t cap, int timeout_ms) = 0;
    virtual bool write(const uint8_t* p, size_t n) = 0;
    // Close from another thread: a blocked read() returns < 0.
    virtual void close() = 0;
};

// A ByteStream over a connected socket fd (RFCOMM on Windows and Linux).
std::unique_ptr<ByteStream> socket_stream(intptr_t fd);

// Pump one phone connection: frames in -> Receiver, STATUS back. Returns when the
// stream closes, misbehaves, stays silent for 10 s, or `stop` is set.
void serve_stream(Receiver* rx, ByteStream& s, const SourceId& src, const std::atomic<bool>& stop);

class BluetoothServer {
public:
    enum class State {
        Starting,
        Unavailable,  // no Bluetooth adapter / stack
        PoweredOff,   // the adapter is switched off
        Ready,        // service published, waiting for the phone
        Failed,       // could not publish the service (see detail())
    };

    BluetoothServer(Receiver* rx, EventLog* log) : rx_(rx), log_(log) {}
    ~BluetoothServer();
    BluetoothServer(const BluetoothServer&) = delete;
    BluetoothServer& operator=(const BluetoothServer&) = delete;

    // Publish the service and accept phones in the background. Keeps retrying,
    // so an adapter that is plugged in or switched on later is picked up.
    void start();
    void stop();

    State state() const { return state_.load(); }
    std::string adapter_name() const;  // the PC's Bluetooth name, as the phone lists it
    std::string detail() const;        // why the state is Unavailable / Failed
    int connections() const { return active_.load(); }

    // Platform backends hand every accepted connection to this.
    void accept(std::unique_ptr<ByteStream> s, const std::string& peer_name);

private:
    void platform_loop();  // bluetooth_<os>.cpp: publish, accept, retry until stop_
    void set_state(State s, const std::string& detail = {});
    void set_adapter_name(const std::string& n);
    bool sleep_unless_stopped(int ms);

    Receiver* rx_;
    EventLog* log_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Starting};
    std::atomic<int> active_{0};
    mutable std::mutex mu_;
    std::string adapter_name_, detail_;
    std::vector<ByteStream*> streams_;  // open connections, closed by stop()
};

// Exposed for tests: split a byte stream into length-prefixed frames.
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
