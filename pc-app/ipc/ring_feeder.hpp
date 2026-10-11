// Feeds the driver's shared ring (ring.h) from an AdaptivePlayout, paced by the
// driver's own clock.
//
// The driver drains the ring at its sample clock, which drifts against the
// phone's like any sound card does. So the ring is not written as audio
// arrives: a feeder keeps it topped up to a small target (~20 ms) by pulling
// from AdaptivePlayout, i.e. exactly as fast as the driver consumes. The
// playout buffer then does what it does for a real device: holds its latency
// target and absorbs the drift by resampling (see playout.hpp).
//
// Platform-neutral so it can be tested without the driver; on Windows
// DriverFeed (driver_feed.hpp) maps the real section and owns one of these.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "phonemic/playout.hpp"
#include "phonemic/receiver.hpp"
#include "ring_producer.hpp"

namespace phonemic {

class RingFeeder final : public AudioSinkIface {
public:
    // `ring` must stay mapped (and valid) for this object's lifetime.
    explicit RingFeeder(PHONEMIC_RING* ring, uint32_t ring_target_ms = 20);
    ~RingFeeder() override;
    RingFeeder(const RingFeeder&) = delete;
    RingFeeder& operator=(const RingFeeder&) = delete;

    // Background thread that calls pump() every couple of milliseconds.
    void start();
    void stop();
    // One top-up step: bring the ring back up to its target from the playout
    // buffer. Called by the thread; tests call it directly.
    void pump();

    // AudioSinkIface (receive thread)
    void write(const float* s, size_t n) override { playout_.write(s, n); }
    void flush() override { playout_.request_flush(); }
    uint32_t buffered_ms() const override;

    void set_latency_ms(uint32_t ms) { playout_.set_target_ms(ms); }
    AdaptivePlayout::Stats stats() const { return playout_.stats(); }
    // Samples the driver has taken out of the ring so far.
    uint64_t consumed() const { return ring_->read_index; }

private:
    PHONEMIC_RING* ring_;
    RingProducer producer_;
    AdaptivePlayout playout_;
    size_t target_;  // ring fill to keep (samples)
    std::vector<float> f_;
    std::vector<int16_t> pcm_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace phonemic
