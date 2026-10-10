// Shared-memory ring: the single audio data path between the user-mode core
// (producer) and the kernel ACX driver (consumer). See ipc/README.md for the
// rationale (ring over per-buffer IOCTL). This header is the canonical contract;
// the C++ producer (ring_producer.hpp) static_asserts this exact layout.
//
// Discipline: single-producer (user mode) / single-consumer (driver). Indices
// are monotonically increasing sample counters; the slot is `index % capacity`.
// The producer publishes samples then releases `write_index` with a release
// store; the consumer reads `write_index` with an acquire load before reading
// samples. This is the standard SPSC lock-free handoff, valid across the
// user/kernel boundary because the section is mapped into both.
#pragma once

#if defined(_KERNEL_MODE)
// The kernel CRT has no <stdint.h>: use the WDK's fixed-width types.
#include <ntdef.h>
typedef UINT8 phonemic_u8;
typedef INT16 phonemic_i16;
typedef UINT32 phonemic_u32;
typedef UINT64 phonemic_u64;
#else
#include <stdint.h>
typedef uint8_t phonemic_u8;
typedef int16_t phonemic_i16;
typedef uint32_t phonemic_u32;
typedef uint64_t phonemic_u64;
#endif

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
    phonemic_u32 abi_version;   // == PHONEMIC_RING_ABI_VERSION
    phonemic_u32 capacity;      // == PHONEMIC_RING_CAPACITY (samples)
    phonemic_u32 sample_rate;   // == PHONEMIC_SAMPLE_RATE
    phonemic_u32 channels;      // == PHONEMIC_CHANNELS

    // Cache-line separation so producer and consumer don't false-share the two
    // hot indices.
    phonemic_u8  _pad0[64 - 16];
    volatile phonemic_u64 write_index;  // producer-owned: total samples written
    phonemic_u8  _pad1[64 - 8];
    volatile phonemic_u64 read_index;   // consumer-owned: total samples read
    phonemic_u8  _pad2[64 - 8];

    phonemic_i16  samples[PHONEMIC_RING_CAPACITY];
} PHONEMIC_RING;
#pragma pack(pop)

// Samples available to the consumer right now.
static inline phonemic_u64 phonemic_ring_available(const PHONEMIC_RING* r) {
    return r->write_index - r->read_index;
}
