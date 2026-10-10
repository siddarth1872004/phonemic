#include "phonemic/util.hpp"

#include <cstdio>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace phonemic {

bool random_bytes(void* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    auto* p = static_cast<unsigned char*>(out);
    while (got < n) {
        const ssize_t r = ::read(fd, p + got, n - got);
        if (r <= 0) {
            ::close(fd);
            return false;
        }
        got += static_cast<size_t>(r);
    }
    ::close(fd);
    return true;
#endif
}

uint32_t random_u32_nonzero() {
    uint32_t v = 0;
    while (v == 0) {
        if (!random_bytes(&v, sizeof v)) v = static_cast<uint32_t>(mono_us() * 2654435761u) | 1u;
    }
    return v;
}

std::string random_token(size_t len) {
    static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // 31 chars, no 0/o/1/l/i
    std::string out;
    while (out.size() < len) {
        unsigned char b[32];
        if (!random_bytes(b, sizeof b)) break;
        for (unsigned char c : b) {
            if (c >= 31 * 8) continue;  // reject to avoid modulo bias
            out.push_back(kAlphabet[c % 31]);
            if (out.size() == len) break;
        }
    }
    return out;
}

bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

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
