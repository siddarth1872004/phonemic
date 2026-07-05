// Shared-memory ring: the single audio data path between the user-mode core
// (producer) and the kernel ACX driver (consumer). See ipc/README.md for the
// rationale (ring over per-buffer IOCTL). This header is the canonical contract;
// the Rust side mirrors this layout exactly (see ring_mirror.rs).
//
// Discipline: single-producer (user mode) / single-consumer (driver). Indices
// are monotonically increasing sample counters; the slot is `index % capacity`.
// The producer publishes samples then releases `write_index` with a release
// store; the consumer reads `write_index` with an acquire load before reading
// samples. This is the standard SPSC lock-free handoff, valid across the
// user/kernel boundary because the section is mapped into both.
#pragma once

#include <stdint.h>

// 1 second of mono 48 kHz PCM16. Power-of-two would let us mask instead of mod;
// kept round for clarity — revisit if profiling says the `%` matters.
#define PHONEMIC_RING_CAPACITY 48000u
#define PHONEMIC_SAMPLE_RATE   48000u
#define PHONEMIC_CHANNELS      1u

// Bumped if this layout ever changes so both sides can reject a mismatch during
// the mapping handshake (the establishing IOCTL exchanges this).
#define PHONEMIC_RING_ABI_VERSION 1u

#pragma pack(push, 8)
typedef struct _PHONEMIC_RING {
    uint32_t abi_version;   // == PHONEMIC_RING_ABI_VERSION
    uint32_t capacity;      // == PHONEMIC_RING_CAPACITY (samples)
    uint32_t sample_rate;   // == PHONEMIC_SAMPLE_RATE
    uint32_t channels;      // == PHONEMIC_CHANNELS

    // Cache-line separation so producer and consumer don't false-share the two
    // hot indices.
    uint8_t  _pad0[64 - 16];
    volatile uint64_t write_index;  // producer-owned: total samples written
    uint8_t  _pad1[64 - 8];
    volatile uint64_t read_index;   // consumer-owned: total samples read
    uint8_t  _pad2[64 - 8];

    int16_t  samples[PHONEMIC_RING_CAPACITY];
} PHONEMIC_RING;
#pragma pack(pop)

// Samples available to the consumer right now.
static inline uint64_t phonemic_ring_available(const PHONEMIC_RING* r) {
    return r->write_index - r->read_index;
}
