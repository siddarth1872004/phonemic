// Adaptive playout buffer: the hand-off from the network thread to a realtime
// audio-device callback, with clock-drift compensation.
//
// The phone's sample clock and the PC sound card's clock never run at exactly
// the same rate (typically 20-200 ppm apart). With a plain FIFO the buffer
// therefore slowly fills (latency creeps up, then overflows and glitches) or
// drains (periodic underrun clicks). This buffer instead:
//
//  * primes to a target latency before playing,
//  * continuously measures its fill level and nudges the read rate by at most
//    +/-0.4 % (resampling with linear interpolation — inaudible) to hold the
//    fill at the target, and
//  * hard-resyncs if the fill ever runs far past the target (e.g. after the
//    sender was paused), so latency is bounded.
//
// write() is called by one producer thread, render() by the audio callback;
// they share only a lock-free SPSC ring and atomics. render() never allocates.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "phonemic/spsc_ring.hpp"

namespace phonemic {

class AdaptivePlayout {
public:
    explicit AdaptivePlayout(uint32_t sample_rate = 48000, uint32_t capacity_ms = 1000);

    // Target buffering latency (clamped to 10..500 ms). Safe from any thread.
    void set_target_ms(uint32_t ms);
    uint32_t target_ms() const { return target_ms_.load(std::memory_order_relaxed); }

    // Producer side. Returns samples accepted (excess is dropped and counted).
    size_t write(const float* s, size_t n);
    // Producer side: ask the consumer to drop everything and re-prime (e.g. a new
    // phone session started).
    void request_flush() { flush_req_.store(true, std::memory_order_release); }

    // Consumer side (audio callback): produce exactly n mono samples.
    void render(float* out, size_t n);

    struct Stats {
        float fill_ms = 0;  // current buffered latency
        float ratio = 1;    // current drift-correction read ratio
        uint64_t underruns = 0, resyncs = 0, overflow_samples = 0;
        bool playing = false;
    };
    Stats stats() const;

private:
    bool refill_stage();
    size_t buffered() const;

    uint32_t rate_;
    SpscRing<float> ring_;
    std::atomic<uint32_t> target_ms_{40};
    std::atomic<bool> flush_req_{false};

    // Consumer-only state.
    std::vector<float> stage_;
    size_t stage_len_ = 0, stage_pos_ = 0;
    bool running_ = false;
    double frac_ = 1.0;
    float prev_ = 0, cur_ = 0;
    double fill_avg_ = 0;

    // Stats (written by consumer except overflow).
    std::atomic<float> st_fill_ms_{0}, st_ratio_{1};
    std::atomic<uint64_t> st_underruns_{0}, st_resyncs_{0}, st_overflow_{0};
    std::atomic<bool> st_playing_{false};
};

}  // namespace phonemic
