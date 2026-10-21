#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace phonemic {

// Monotonic milliseconds (for timeouts / "last seen"); never jumps with the wall clock.
inline uint64_t mono_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
inline uint64_t mono_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
inline uint64_t wall_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// Thread-safe "recent activity" log shown in the UI and printed by the CLI.
class EventLog {
public:
    explicit EventLog(size_t max_entries = 60, bool echo_stderr = false)
        : max_(max_entries), echo_(echo_stderr) {}
    void add(const std::string& msg);
    std::vector<std::string> recent() const;  // oldest first, "HH:MM:SS  msg"
    uint64_t version() const;

private:
    mutable std::mutex mu_;
    std::deque<std::string> entries_;
    size_t max_;
    bool echo_;
    uint64_t version_ = 0;
};

}  // namespace phonemic
