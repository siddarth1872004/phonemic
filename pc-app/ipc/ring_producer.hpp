// User-mode producer for the core -> driver shared-memory ring (ring.h).
//
// ring.h is the canonical layout shared with the kernel driver; this is the
// C++ writer side. Single producer (this process) / single consumer (driver):
// samples are stored first, then write_index is published with a release
// fence; the consumer's read_index is read with an acquire fence before
// trusting the free space.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ring.h"

namespace phonemic {

static_assert(offsetof(PHONEMIC_RING, write_index) == 64, "ring.h layout changed");
static_assert(offsetof(PHONEMIC_RING, read_index) == 128, "ring.h layout changed");
static_assert(offsetof(PHONEMIC_RING, samples) == 192, "ring.h layout changed");

inline void ring_init_header(PHONEMIC_RING* r) {
    r->abi_version = PHONEMIC_RING_ABI_VERSION;
    r->capacity = PHONEMIC_RING_CAPACITY;
    r->sample_rate = PHONEMIC_SAMPLE_RATE;
    r->channels = PHONEMIC_CHANNELS;
    r->write_index = 0;
    r->read_index = 0;
}

inline bool ring_header_ok(const PHONEMIC_RING* r) {
    return r->abi_version == PHONEMIC_RING_ABI_VERSION && r->capacity == PHONEMIC_RING_CAPACITY &&
           r->sample_rate == PHONEMIC_SAMPLE_RATE && r->channels == PHONEMIC_CHANNELS;
}

class RingProducer {
public:
    // `ring` must stay mapped for this object's lifetime, and this must be the
    // only producer for it.
    explicit RingProducer(PHONEMIC_RING* ring) : ring_(ring) {}

    uint64_t occupied() const {
        const uint64_t read = ring_->read_index;  // volatile load
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t used = ring_->write_index - read;
        // A misbehaving consumer can't make us believe the ring is over-full.
        return used > PHONEMIC_RING_CAPACITY ? PHONEMIC_RING_CAPACITY : used;
    }
    uint64_t vacant() const { return PHONEMIC_RING_CAPACITY - occupied(); }

    // Push as many samples as fit; returns the count accepted (never blocks).
    size_t push(const int16_t* samples, size_t n) {
        const size_t room = static_cast<size_t>(vacant());
        if (n > room) n = room;
        if (n == 0) return 0;
        const uint64_t w = ring_->write_index;
        for (size_t i = 0; i < n; ++i) ring_->samples[(w + i) % PHONEMIC_RING_CAPACITY] = samples[i];
        std::atomic_thread_fence(std::memory_order_release);
        ring_->write_index = w + n;  // publish
        return n;
    }

private:
    PHONEMIC_RING* ring_;
};

}  // namespace phonemic
