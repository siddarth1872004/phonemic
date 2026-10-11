#include "ring_feeder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace phonemic {

RingFeeder::RingFeeder(PHONEMIC_RING* ring, uint32_t ring_target_ms)
    : ring_(ring),
      producer_(ring),
      playout_(PHONEMIC_SAMPLE_RATE, 1000),
      target_(std::min<size_t>(PHONEMIC_SAMPLE_RATE / 1000 * ring_target_ms, PHONEMIC_RING_CAPACITY / 2)) {
    f_.resize(target_);
    pcm_.resize(target_);
}

RingFeeder::~RingFeeder() { stop(); }

void RingFeeder::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
#ifdef _WIN32
        // 1 ms scheduler ticks while feeding (the default is 15.6 ms, longer
        // than the ring target).
        timeBeginPeriod(1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        while (running_.load(std::memory_order_relaxed)) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    });
}

void RingFeeder::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void RingFeeder::pump() {
    const uint64_t occupied = producer_.occupied();
    if (occupied >= target_) return;
    const size_t n = target_ - static_cast<size_t>(occupied);
    playout_.render(f_.data(), n);
    for (size_t i = 0; i < n; ++i)
        pcm_[i] = static_cast<int16_t>(std::lrint(std::clamp(f_[i], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
    producer_.push(pcm_.data(), n);
}

uint32_t RingFeeder::buffered_ms() const {
    return static_cast<uint32_t>(playout_.stats().fill_ms) +
           static_cast<uint32_t>(producer_.occupied() * 1000 / PHONEMIC_SAMPLE_RATE);
}

}  // namespace phonemic
