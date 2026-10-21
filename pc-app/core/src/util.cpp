#include "phonemic/util.hpp"

#include <cstdio>
#include <ctime>

namespace phonemic {

void EventLog::add(const std::string& msg) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    std::string line = std::string(ts) + "  " + msg;
    if (echo_) std::fprintf(stderr, "%s\n", line.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    entries_.push_back(std::move(line));
    while (entries_.size() > max_) entries_.pop_front();
    ++version_;
}

std::vector<std::string> EventLog::recent() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {entries_.begin(), entries_.end()};
}

uint64_t EventLog::version() const {
    std::lock_guard<std::mutex> lk(mu_);
    return version_;
}

}  // namespace phonemic
