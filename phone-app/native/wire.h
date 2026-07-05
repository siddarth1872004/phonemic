// PhoneMic wire framing (C, no Android/Oboe deps) — the phone-side mirror of the
// /protocol Rust crate. Kept dependency-free so it can be host-compiled and
// cross-checked against the Rust decoder (see native/test/), which is how we
// guarantee the C++ sender and Rust receiver agree on the byte layout without a
// device in the loop.
//
// Layout and little-endian convention: see docs/PROTOCOL.md.
#pragma once

#include <stdint.h>
#include <string.h>

#define PM_MAGIC        0x4D50  // "PM"
#define PM_VERSION      1
#define PM_CODEC_PCM16  0
#define PM_CODEC_OPUS   1
#define PM_HEADER_LEN   18

static inline void pm_w16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void pm_w32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}
static inline void pm_w64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

// Encode header + payload into `out` (which must hold >= PM_HEADER_LEN + len
// bytes). Returns the total number of bytes written.
static inline int pm_encode(uint8_t codec, uint32_t seq, uint64_t timestamp_us,
                            const uint8_t* payload, uint16_t len, uint8_t* out) {
    pm_w16(out + 0, PM_MAGIC);
    out[2] = PM_VERSION;
    out[3] = codec;
    pm_w32(out + 4, seq);
    pm_w64(out + 8, timestamp_us);
    pm_w16(out + 16, len);
    memcpy(out + PM_HEADER_LEN, payload, len);
    return PM_HEADER_LEN + len;
}
