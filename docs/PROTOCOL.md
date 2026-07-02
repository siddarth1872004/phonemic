# PhoneMic wire protocol

This document is the human-readable companion to the `phonemic-protocol` crate
(`/protocol`). **The crate is the source of truth.** If they ever disagree, the
code wins and this file is the bug.

## Framing

Every UDP datagram (Phases 0–2) and every RFCOMM frame (Phase 3) is a fixed
18-byte header followed by `payload_len` bytes of audio.

```
offset  size  field         type   notes
  0      2    magic         u16    protocol id, "PM". Reject anything else.
  2      1    version       u8     protocol version (currently 1)
  3      1    codec         u8     0 = PCM16, 1 = Opus
  4      4    seq           u32    monotonic sequence number
  8      8    timestamp_us  u64    capture time (microseconds), phone clock
 16      2    payload_len   u16    number of payload bytes that follow
 18    ...    payload              payload_len bytes of (encoded) audio
```

Constants: `MAGIC = 0x4D50` ("PM"), `PROTOCOL_VERSION = 1`, `HEADER_LEN = 18`.

## Endianness

All multi-byte integers are **little-endian**, including `magic`. Both target
CPUs — ARM on the phone, x86-64 on the PC — are little-endian, so this is a
zero-cost choice with no byte swapping on the hot path. This deliberately breaks
the "network byte order" (big-endian) convention; it is documented here and in
the crate so it never becomes a silent surprise.

`magic` little-endian `0x4D50` places bytes `0x50 0x4D` = ASCII `"PM"` first on
the wire, which is convenient when eyeballing a packet capture.

## Payload

- **PCM16 (codec 0):** raw signed 16-bit samples, little-endian, mono, 48 kHz.
  Used in Phase 0 only. Payload length must be even (whole samples).
- **Opus (codec 1):** one Opus frame. Introduced in Phase 1 for its low frame
  latency (2.5–20 ms) and packet-loss concealment. Recommended frame size:
  10 ms (480 samples @ 48 kHz) as the latency/robustness starting point.

## Design rules

- **Loss tolerance, not retransmit.** This is a real-time stream. A missing
  `seq` triggers Opus packet-loss concealment on the PC, never a retransmit
  request. There is no ACK.
- **Jitter buffer (PC side, Phase 1).** A small ring reorders by `seq` /
  `timestamp_us` and smooths network jitter before the decoder. Target depth is
  configurable, starting at 2–3 frames.
- **Versioning.** `version` lets the protocol evolve without silently
  mis-parsing an old client. A receiver rejects unsupported versions
  (`ProtocolError::UnsupportedVersion`) rather than crashing; a future version
  can add a renegotiation handshake. Decoding is total: every malformed or
  truncated datagram maps to a `ProtocolError` variant and is dropped, never
  panics, never allocates.

## Validation performed by `decode`

In order: length ≥ 18 → magic matches → version supported → codec known →
declared `payload_len` exactly equals the bytes present (trailing garbage and
truncation both rejected). See the unit tests in `/protocol/src/lib.rs`.
