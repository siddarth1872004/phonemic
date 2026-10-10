// Lock-free single-producer / single-consumer ring buffer.
//
// The handoff between the network thread (producer) and the realtime audio
// callback (consumer). The audio thread must never block on a lock held by the
// network thread, so the only shared state is two monotonically increasing
// indices published with release/acquire ordering.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phonemic {

template <typename T>
class SpscRing {
public:
    // Capacity is rounded up to a power of two so slots are a mask, not a modulo.
    explicit SpscRing(size_t min_capacity) {
        size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        buf_.assign(cap, T{});
        mask_ = cap - 1;
    }

    size_t capacity() const { return buf_.size(); }

    // Consumer-side and producer-side views of the fill level. Either side may
    // call these; the answer is a consistent snapshot for that side.
    size_t size() const {
        const uint64_t w = write_.load(std::memory_order_acquire);
        const uint64_t r = read_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }
    size_t free_space() const { return capacity() - size(); }

    // Producer: push up to n items, returns how many fit (never blocks).
    size_t push(const T* src, size_t n) {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t room = capacity() - static_cast<size_t>(w - r);
        if (n > room) n = room;
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer: pop up to n items into dst, returns how many were available.
    size_t pop(T* dst, size_t n) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        const size_t avail = static_cast<size_t>(w - r);
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer: discard up to n items (latency resync).
    size_t skip(size_t n) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        const size_t avail = static_cast<size_t>(w - r);
        if (n > avail) n = avail;
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    // Separate cache lines so producer and consumer don't false-share.
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

}  // namespace phonemic
