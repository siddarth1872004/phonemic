/*
 * PhoneMic wire protocol — THE single source of truth.
 *
 * This header is compiled, unchanged, into both ends: the Android native audio
 * engine (phone-app/native) and the C++ desktop app (pc-app). There is no second
 * implementation that could drift. It is plain C99 (header-only, no allocation,
 * no dependencies) so it also builds in C test harnesses.
 *
 * Every datagram starts with a 4-byte common prefix:
 *
 *   offset size field
 *     0     2   magic     u16  0x4D50 — ASCII "PM" on the wire
 *     2     1   version   u8   2 (version 1 = legacy 18-byte audio header)
 *     3     1   kind      u8   PM_KIND_*
 *
 * All multi-byte integers are LITTLE-ENDIAN (both target CPUs are LE; see
 * docs/PROTOCOL.md). The encoders/decoders below are byte-wise so they are
 * correct on any host regardless.
 *
 * AUDIO (kind 0, phone -> PC), 28-byte header:
 *     4     1   codec         PM_CODEC_*
 *     5     1   flags         bit0 = payload encrypted (XChaCha20-Poly1305)
 *     6     2   payload_len   bytes after the header (incl. 16-byte tag if encrypted)
 *     8     4   session_id    random per stream start, never 0
 *    12     4   seq           per-session packet counter, starts at 0
 *    16     8   timestamp_us  sender's monotonic capture clock
 *    24     4   sample_rate   Hz of the PCM (receiver resamples if != 48000)
 *    28   ...   payload       mono PCM16 LE, or one Opus packet (or ciphertext||tag)
 *
 * HELLO (kind 1, phone -> broadcast UDP:4010): discovery probe, 12 bytes.
 *     4     4   nonce         echoed in the ANNOUNCE so replies can be matched
 *     8     4   reserved      0
 *
 * ANNOUNCE (kind 2, PC -> phone): discovery reply, 12 + name_len bytes.
 *     4     4   nonce         copied from the HELLO
 *     8     2   audio_port    UDP port the PC receives audio on
 *    10     1   flags         bit0 = PIN required, bit1 = busy (someone streaming)
 *    11     1   name_len      <= PM_MAX_NAME
 *    12   ...   name          UTF-8 host name (not NUL-terminated)
 *
 * STATUS (kind 3, PC -> phone, ~2 Hz): receiver feedback, 24 bytes.
 *     4     1   state         PM_STATUS_*
 *     5     1   reserved      0
 *     6     2   buffer_ms     current PC playout buffer (latency contribution)
 *     8     4   session_id    the session this report is about
 *    12     4   received      audio packets accepted this session
 *    16     4   lost          packets concealed (lost) this session
 *    20     4   reserved      0
 *
 * Legacy v1 audio (18-byte header: magic, version=1, codec|0x80 encrypted, seq
 * u32, timestamp u64, payload_len u16) is still DECODED so an old phone app keeps
 * working in plaintext; it is never encoded any more.
 */
#ifndef PHONEMIC_WIRE_H
#define PHONEMIC_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_MAGIC            0x4D50u
#define PM_VERSION          2
#define PM_VERSION_LEGACY   1

#define PM_KIND_AUDIO       0
#define PM_KIND_HELLO       1
#define PM_KIND_ANNOUNCE    2
#define PM_KIND_STATUS      3

#define PM_CODEC_PCM16      0
#define PM_CODEC_OPUS       1 /* one Opus packet per 10 ms frame, decoded at 48 kHz */

#define PM_FLAG_ENCRYPTED   0x01

#define PM_AUDIO_HEADER_LEN   28
#define PM_LEGACY_HEADER_LEN  18
#define PM_HELLO_LEN          12
#define PM_ANNOUNCE_MIN_LEN   12
#define PM_STATUS_LEN         24
#define PM_MAX_NAME           63

#define PM_DEFAULT_PORT       4010
#define PM_SAMPLE_RATE        48000
#define PM_MIN_SAMPLE_RATE    8000
#define PM_MAX_SAMPLE_RATE    96000
/* 10 ms frames: 480 samples at 48 kHz = 960 payload bytes; stays under the MTU. */
#define PM_FRAME_MS           10
/* Largest payload either end accepts (keeps datagrams well under 64 KiB). */
#define PM_MAX_PAYLOAD        4096
#define PM_MAX_DATAGRAM       (PM_AUDIO_HEADER_LEN + PM_MAX_PAYLOAD)

#define PM_ANNOUNCE_FLAG_PIN  0x01
#define PM_ANNOUNCE_FLAG_BUSY 0x02

/* STATUS.state */
#define PM_STATUS_OK            0 /* receiving and playing */
#define PM_STATUS_PIN_REQUIRED  1 /* PC has a PIN; phone sent plaintext */
#define PM_STATUS_PIN_MISSING   2 /* phone encrypts; PC has no PIN set */
#define PM_STATUS_PIN_WRONG     3 /* decryption failed: PINs differ */
#define PM_STATUS_BUSY          4 /* another device is already streaming */
#define PM_STATUS_UNSUPPORTED   5 /* codec / sample rate not supported */

typedef enum pm_result {
    PM_OK = 0,
    PM_ERR_TOO_SHORT = -1,
    PM_ERR_BAD_MAGIC = -2,
    PM_ERR_UNSUPPORTED_VERSION = -3,
    PM_ERR_WRONG_KIND = -4,
    PM_ERR_UNKNOWN_CODEC = -5,
    PM_ERR_LEN_MISMATCH = -6,
    PM_ERR_OUTPUT_TOO_SMALL = -7,
    PM_ERR_PAYLOAD_TOO_LARGE = -8,
    PM_ERR_BAD_FIELD = -9
} pm_result;

/* Decoded AUDIO header (v2, or a legacy v1 header mapped onto it). */
typedef struct pm_audio_header {
    uint8_t  version;      /* 2, or 1 for legacy */
    uint8_t  codec;        /* PM_CODEC_* */
    uint8_t  flags;        /* PM_FLAG_* */
    uint16_t payload_len;
    uint32_t session_id;   /* 0 for legacy v1 */
    uint32_t seq;
    uint64_t timestamp_us;
    uint32_t sample_rate;  /* 48000 for legacy v1 */
    uint16_t header_len;   /* bytes of header on the wire (the AEAD's AAD) */
} pm_audio_header;

typedef struct pm_announce {
    uint32_t nonce;
    uint16_t audio_port;
    uint8_t  flags;
    uint8_t  name_len;
    char     name[PM_MAX_NAME + 1]; /* NUL-terminated after decode */
} pm_announce;

typedef struct pm_status {
    uint8_t  state;
    uint16_t buffer_ms;
    uint32_t session_id;
    uint32_t received;
    uint32_t lost;
} pm_status;

/* ---- byte helpers -------------------------------------------------------- */

static inline void pm_w16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static inline void pm_w32(uint8_t* p, uint32_t v) {
    int i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void pm_w64(uint8_t* p, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint16_t pm_r16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t pm_r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t pm_r64(const uint8_t* p) {
    return (uint64_t)pm_r32(p) | ((uint64_t)pm_r32(p + 4) << 32);
}

static inline void pm_write_prefix(uint8_t* out, uint8_t kind) {
    pm_w16(out, (uint16_t)PM_MAGIC);
    out[2] = PM_VERSION;
    out[3] = kind;
}

/* Validate the common prefix. On success stores version and kind. A legacy v1
 * datagram reports kind PM_KIND_AUDIO (v1 had no kind byte; its byte 3 is the
 * codec). */
static inline pm_result pm_peek(const uint8_t* buf, size_t len, uint8_t* version,
                                uint8_t* kind) {
    if (len < 4) return PM_ERR_TOO_SHORT;
    if (pm_r16(buf) != PM_MAGIC) return PM_ERR_BAD_MAGIC;
    if (buf[2] == PM_VERSION_LEGACY) {
        *version = PM_VERSION_LEGACY;
        *kind = PM_KIND_AUDIO;
        return PM_OK;
    }
    if (buf[2] != PM_VERSION) return PM_ERR_UNSUPPORTED_VERSION;
    *version = PM_VERSION;
    *kind = buf[3];
    return PM_OK;
}

/* ---- AUDIO ---------------------------------------------------------------- */

/* Write a 28-byte v2 AUDIO header. `h->payload_len` is written as given (for
 * an encrypted packet it must already include the tag). */
static inline void pm_write_audio_header(uint8_t out[PM_AUDIO_HEADER_LEN],
                                         const pm_audio_header* h) {
    pm_write_prefix(out, PM_KIND_AUDIO);
    out[4] = h->codec;
    out[5] = h->flags;
    pm_w16(out + 6, h->payload_len);
    pm_w32(out + 8, h->session_id);
    pm_w32(out + 12, h->seq);
    pm_w64(out + 16, h->timestamp_us);
    pm_w32(out + 24, h->sample_rate);
}

/* Encode a full plaintext AUDIO packet. Returns total bytes written (> 0) or a
 * negative pm_result. payload_len in `h` is ignored and computed from `len`, so a
 * packet whose declared length disagrees with its contents cannot be produced. */
static inline int pm_encode_audio(uint8_t* out, size_t out_cap, const pm_audio_header* h,
                                  const uint8_t* payload, size_t len) {
    pm_audio_header hh;
    if (len > PM_MAX_PAYLOAD) return PM_ERR_PAYLOAD_TOO_LARGE;
    if (out_cap < PM_AUDIO_HEADER_LEN + len) return PM_ERR_OUTPUT_TOO_SMALL;
    hh = *h;
    hh.payload_len = (uint16_t)len;
    pm_write_audio_header(out, &hh);
    if (len) memcpy(out + PM_AUDIO_HEADER_LEN, payload, len);
    return (int)(PM_AUDIO_HEADER_LEN + len);
}

/* Decode one AUDIO datagram (v2 or legacy v1). `buf` must hold exactly one
 * datagram: truncation AND trailing bytes are both rejected. On success `*payload`
 * points into `buf`. Never reads out of bounds, never allocates. */
static inline pm_result pm_decode_audio(const uint8_t* buf, size_t len, pm_audio_header* h,
                                        const uint8_t** payload) {
    uint8_t version = 0, kind = 0;
    pm_result r = pm_peek(buf, len, &version, &kind);
    if (r != PM_OK) return r;
    if (kind != PM_KIND_AUDIO) return PM_ERR_WRONG_KIND;

    if (version == PM_VERSION_LEGACY) {
        if (len < PM_LEGACY_HEADER_LEN) return PM_ERR_TOO_SHORT;
        h->version = PM_VERSION_LEGACY;
        h->codec = (uint8_t)(buf[3] & 0x7F);
        h->flags = (buf[3] & 0x80) ? PM_FLAG_ENCRYPTED : 0;
        h->session_id = 0;
        h->seq = pm_r32(buf + 4);
        h->timestamp_us = pm_r64(buf + 8);
        h->payload_len = pm_r16(buf + 16);
        h->sample_rate = PM_SAMPLE_RATE;
        h->header_len = PM_LEGACY_HEADER_LEN;
    } else {
        if (len < PM_AUDIO_HEADER_LEN) return PM_ERR_TOO_SHORT;
        h->version = PM_VERSION;
        h->codec = buf[4];
        h->flags = buf[5];
        h->payload_len = pm_r16(buf + 6);
        h->session_id = pm_r32(buf + 8);
        h->seq = pm_r32(buf + 12);
        h->timestamp_us = pm_r64(buf + 16);
        h->sample_rate = pm_r32(buf + 24);
        h->header_len = PM_AUDIO_HEADER_LEN;
        if (h->session_id == 0) return PM_ERR_BAD_FIELD;
        if (h->sample_rate < PM_MIN_SAMPLE_RATE || h->sample_rate > PM_MAX_SAMPLE_RATE)
            return PM_ERR_BAD_FIELD;
        if (h->flags & ~PM_FLAG_ENCRYPTED) return PM_ERR_BAD_FIELD;
    }
    if (h->codec != PM_CODEC_PCM16 && h->codec != PM_CODEC_OPUS) return PM_ERR_UNKNOWN_CODEC;
    if ((size_t)h->payload_len != len - h->header_len) return PM_ERR_LEN_MISMATCH;
    if (h->payload_len > PM_MAX_PAYLOAD) return PM_ERR_PAYLOAD_TOO_LARGE;
    *payload = buf + h->header_len;
    return PM_OK;
}

/* ---- HELLO / ANNOUNCE (discovery) ---------------------------------------- */

static inline int pm_encode_hello(uint8_t out[PM_HELLO_LEN], uint32_t nonce) {
    pm_write_prefix(out, PM_KIND_HELLO);
    pm_w32(out + 4, nonce);
    pm_w32(out + 8, 0);
    return PM_HELLO_LEN;
}

static inline pm_result pm_decode_hello(const uint8_t* buf, size_t len, uint32_t* nonce) {
    uint8_t version = 0, kind = 0;
    pm_result r = pm_peek(buf, len, &version, &kind);
    if (r != PM_OK) return r;
    if (version != PM_VERSION || kind != PM_KIND_HELLO) return PM_ERR_WRONG_KIND;
    if (len != PM_HELLO_LEN) return PM_ERR_LEN_MISMATCH;
    *nonce = pm_r32(buf + 4);
    return PM_OK;
}

/* Returns bytes written or a negative pm_result. Names longer than PM_MAX_NAME
 * bytes are truncated. */
static inline int pm_encode_announce(uint8_t* out, size_t out_cap, const pm_announce* a) {
    size_t n = strlen(a->name);
    if (n > PM_MAX_NAME) n = PM_MAX_NAME;
    if (out_cap < PM_ANNOUNCE_MIN_LEN + n) return PM_ERR_OUTPUT_TOO_SMALL;
    pm_write_prefix(out, PM_KIND_ANNOUNCE);
    pm_w32(out + 4, a->nonce);
    pm_w16(out + 8, a->audio_port);
    out[10] = a->flags;
    out[11] = (uint8_t)n;
    memcpy(out + 12, a->name, n);
    return (int)(PM_ANNOUNCE_MIN_LEN + n);
}

static inline pm_result pm_decode_announce(const uint8_t* buf, size_t len, pm_announce* a) {
    uint8_t version = 0, kind = 0;
    pm_result r = pm_peek(buf, len, &version, &kind);
    if (r != PM_OK) return r;
    if (version != PM_VERSION || kind != PM_KIND_ANNOUNCE) return PM_ERR_WRONG_KIND;
    if (len < PM_ANNOUNCE_MIN_LEN) return PM_ERR_TOO_SHORT;
    a->nonce = pm_r32(buf + 4);
    a->audio_port = pm_r16(buf + 8);
    a->flags = buf[10];
    a->name_len = buf[11];
    if (a->name_len > PM_MAX_NAME) return PM_ERR_BAD_FIELD;
    if (len != (size_t)PM_ANNOUNCE_MIN_LEN + a->name_len) return PM_ERR_LEN_MISMATCH;
    memcpy(a->name, buf + 12, a->name_len);
    a->name[a->name_len] = '\0';
    return PM_OK;
}

/* ---- STATUS (receiver feedback) ------------------------------------------ */

static inline int pm_encode_status(uint8_t out[PM_STATUS_LEN], const pm_status* s) {
    pm_write_prefix(out, PM_KIND_STATUS);
    out[4] = s->state;
    out[5] = 0;
    pm_w16(out + 6, s->buffer_ms);
    pm_w32(out + 8, s->session_id);
    pm_w32(out + 12, s->received);
    pm_w32(out + 16, s->lost);
    pm_w32(out + 20, 0);
    return PM_STATUS_LEN;
}

static inline pm_result pm_decode_status(const uint8_t* buf, size_t len, pm_status* s) {
    uint8_t version = 0, kind = 0;
    pm_result r = pm_peek(buf, len, &version, &kind);
    if (r != PM_OK) return r;
    if (version != PM_VERSION || kind != PM_KIND_STATUS) return PM_ERR_WRONG_KIND;
    if (len != PM_STATUS_LEN) return PM_ERR_LEN_MISMATCH;
    s->state = buf[4];
    s->buffer_ms = pm_r16(buf + 6);
    s->session_id = pm_r32(buf + 8);
    s->received = pm_r32(buf + 12);
    s->lost = pm_r32(buf + 16);
    return PM_OK;
}

static inline const char* pm_result_str(pm_result r) {
    switch (r) {
        case PM_OK: return "ok";
        case PM_ERR_TOO_SHORT: return "too short";
        case PM_ERR_BAD_MAGIC: return "bad magic";
        case PM_ERR_UNSUPPORTED_VERSION: return "unsupported version";
        case PM_ERR_WRONG_KIND: return "wrong kind";
        case PM_ERR_UNKNOWN_CODEC: return "unknown codec";
        case PM_ERR_LEN_MISMATCH: return "length mismatch";
        case PM_ERR_OUTPUT_TOO_SMALL: return "output too small";
        case PM_ERR_PAYLOAD_TOO_LARGE: return "payload too large";
        case PM_ERR_BAD_FIELD: return "bad field";
    }
    return "unknown";
}

#ifdef __cplusplus
}
#endif

#endif /* PHONEMIC_WIRE_H */
