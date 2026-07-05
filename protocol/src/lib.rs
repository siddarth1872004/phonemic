//! PhoneMic wire protocol: packet framing shared by the PC app and the phone.
//!
//! This crate is `no_std` (it never allocates and pulls in no dependencies) so
//! the exact same encode/decode logic can be compiled natively for the Rust PC
//! side *and* cross-compiled into the Android native layer (via cbindgen/JNI).
//! Keeping a single source of truth for the packet format is the whole point.
//!
//! # Framing
//!
//! Every UDP datagram / RFCOMM frame is an 18-byte header followed by
//! `payload_len` bytes of (optionally encoded) audio:
//!
//! ```text
//! offset  size  field
//!   0      2    magic         u16   protocol identifier, reject anything else
//!   2      1    version       u8    protocol version
//!   3      1    codec         u8    0 = PCM16, 1 = Opus
//!   4      4    seq           u32   monotonic sequence number
//!   8      8    timestamp_us  u64   capture timestamp, phone-side clock
//!  16      2    payload_len   u16   number of payload bytes that follow
//!  18    ...    payload
//! ```
//!
//! All multi-byte integers are **little-endian**. Both target CPUs (ARM on the
//! phone, x86-64 on the PC) are little-endian, so this is a zero-cost choice on
//! the hot path — no byte swapping in either direction. This is a deliberate
//! break from the "network byte order" convention and is documented here and in
//! `docs/PROTOCOL.md` so it never becomes a silent surprise.

#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_code)]

/// Protocol identifier. ASCII `"PM"` (0x50 0x4D) read little-endian.
/// Any datagram not beginning with this is rejected before we touch it.
pub const MAGIC: u16 = 0x4D50;

/// Current protocol version. Bump on any incompatible framing change.
pub const PROTOCOL_VERSION: u8 = 1;

/// Size of the fixed packet header in bytes.
pub const HEADER_LEN: usize = 18;

/// Audio codec used for a packet's payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Codec {
    /// Raw signed 16-bit PCM samples, little-endian. Used in Phase 0.
    Pcm16 = 0,
    /// Opus-encoded frame. Used from Phase 1 onward (packet-loss concealment).
    Opus = 1,
}

impl Codec {
    /// Map the on-wire codec byte to a [`Codec`], rejecting unknown values.
    #[inline]
    pub fn from_u8(v: u8) -> Result<Self, ProtocolError> {
        match v {
            0 => Ok(Codec::Pcm16),
            1 => Ok(Codec::Opus),
            _ => Err(ProtocolError::UnknownCodec(v)),
        }
    }

    /// The on-wire codec byte.
    #[inline]
    pub fn to_u8(self) -> u8 {
        self as u8
    }
}

/// Everything that can go wrong decoding a packet, or encoding into too small a
/// buffer. Decoding never panics and never allocates: a malformed or truncated
/// datagram becomes one of these variants, which the receiver simply drops.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProtocolError {
    /// Buffer shorter than the fixed header, or shorter than header + payload.
    TooShort,
    /// `magic` field did not match [`MAGIC`].
    BadMagic(u16),
    /// `version` field is not one we speak. Renegotiate; do not crash.
    UnsupportedVersion(u8),
    /// `codec` field is not a value we recognise.
    UnknownCodec(u8),
    /// Declared `payload_len` does not match the bytes actually present.
    PayloadLenMismatch { declared: u16, actual: usize },
    /// The output buffer passed to [`encode`] is too small for header + payload.
    OutputTooSmall { needed: usize, got: usize },
    /// Payload exceeds what the 16-bit `payload_len` field can express.
    PayloadTooLarge(usize),
}

/// The parsed, semantically meaningful contents of a packet header.
///
/// `magic` and the protocol version are validated during [`decode`] and are not
/// stored here — a `PacketHeader` only ever describes a well-formed, supported
/// packet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PacketHeader {
    /// Codec of the payload.
    pub codec: Codec,
    /// Whether the payload is encrypted (XChaCha20-Poly1305, ciphertext||tag).
    /// Carried in bit 7 of the codec byte so old fields are untouched.
    pub encrypted: bool,
    /// Monotonic sequence number, for loss / reorder detection.
    pub seq: u32,
    /// Capture timestamp in microseconds, on the phone's clock.
    pub timestamp_us: u64,
    /// Length of the payload that follows the header.
    pub payload_len: u16,
}

/// Bit of the codec byte that marks an encrypted payload.
const ENCRYPTED_FLAG: u8 = 0x80;

/// Is `version` a protocol version this build can decode?
///
/// Currently only the exact current version. When we evolve the protocol this
/// is where backwards-compatible versions get accepted (and where a negotiation
/// path would branch), rather than silently mis-parsing an old client.
#[inline]
pub fn is_version_supported(version: u8) -> bool {
    version == PROTOCOL_VERSION
}

/// Encode `header`'s fields plus `payload` into `out`, returning the total
/// number of bytes written (`HEADER_LEN + payload.len()`).
///
/// The `payload_len` field of `header` is ignored and recomputed from
/// `payload`, so it is impossible to emit a packet whose declared length
/// disagrees with its contents.
pub fn encode(
    codec: Codec,
    encrypted: bool,
    seq: u32,
    timestamp_us: u64,
    payload: &[u8],
    out: &mut [u8],
) -> Result<usize, ProtocolError> {
    if payload.len() > u16::MAX as usize {
        return Err(ProtocolError::PayloadTooLarge(payload.len()));
    }
    let total = HEADER_LEN + payload.len();
    if out.len() < total {
        return Err(ProtocolError::OutputTooSmall {
            needed: total,
            got: out.len(),
        });
    }

    out[0..2].copy_from_slice(&MAGIC.to_le_bytes());
    out[2] = PROTOCOL_VERSION;
    out[3] = codec.to_u8() | if encrypted { ENCRYPTED_FLAG } else { 0 };
    out[4..8].copy_from_slice(&seq.to_le_bytes());
    out[8..16].copy_from_slice(&timestamp_us.to_le_bytes());
    out[16..18].copy_from_slice(&(payload.len() as u16).to_le_bytes());
    out[HEADER_LEN..total].copy_from_slice(payload);

    Ok(total)
}

/// Decode one packet from `buf`, returning its [`PacketHeader`] and a slice
/// borrowing the payload out of `buf`.
///
/// `buf` is expected to hold exactly one datagram: header + `payload_len` bytes
/// and nothing more. Trailing garbage is treated as corruption
/// ([`ProtocolError::PayloadLenMismatch`]) rather than silently ignored, since
/// on a datagram transport it means the packet is not what it claims to be.
pub fn decode(buf: &[u8]) -> Result<(PacketHeader, &[u8]), ProtocolError> {
    if buf.len() < HEADER_LEN {
        return Err(ProtocolError::TooShort);
    }

    let magic = u16::from_le_bytes([buf[0], buf[1]]);
    if magic != MAGIC {
        return Err(ProtocolError::BadMagic(magic));
    }

    let version = buf[2];
    if !is_version_supported(version) {
        return Err(ProtocolError::UnsupportedVersion(version));
    }

    let codec_byte = buf[3];
    let encrypted = codec_byte & ENCRYPTED_FLAG != 0;
    let codec = Codec::from_u8(codec_byte & !ENCRYPTED_FLAG)?;
    let seq = u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
    let timestamp_us = u64::from_le_bytes([
        buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
    ]);
    let payload_len = u16::from_le_bytes([buf[16], buf[17]]);

    let actual_payload = buf.len() - HEADER_LEN;
    if payload_len as usize != actual_payload {
        return Err(ProtocolError::PayloadLenMismatch {
            declared: payload_len,
            actual: actual_payload,
        });
    }

    let header = PacketHeader {
        codec,
        encrypted,
        seq,
        timestamp_us,
        payload_len,
    };
    Ok((header, &buf[HEADER_LEN..]))
}

/// Interpret a PCM16 payload as a slice of little-endian `i16` samples.
///
/// Returns `None` if the payload length is odd (a partial sample), which would
/// indicate corruption. Zero-copy is not possible across endianness in general,
/// so callers on a big-endian host would need to swap — but both our targets
/// are little-endian (see the module docs), so this is just a length check.
#[inline]
pub fn pcm16_sample_count(payload: &[u8]) -> Option<usize> {
    if payload.len() % 2 == 0 {
        Some(payload.len() / 2)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A payload big enough to exercise the offsets, plus a couple of edge sizes.
    const SAMPLE_PAYLOAD: &[u8] = &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

    fn roundtrip(codec: Codec, seq: u32, ts: u64, payload: &[u8]) {
        let mut buf = [0u8; 512];
        let n = encode(codec, false, seq, ts, payload, &mut buf).expect("encode ok");
        assert_eq!(n, HEADER_LEN + payload.len());

        let (header, decoded_payload) = decode(&buf[..n]).expect("decode ok");
        assert_eq!(header.codec, codec);
        assert!(!header.encrypted);
        assert_eq!(header.seq, seq);
        assert_eq!(header.timestamp_us, ts);
        assert_eq!(header.payload_len as usize, payload.len());
        assert_eq!(decoded_payload, payload);
    }

    #[test]
    fn encrypted_flag_round_trips() {
        let mut buf = [0u8; 64];
        let n = encode(Codec::Opus, true, 1, 2, &[9, 9, 9], &mut buf).unwrap();
        let (header, _) = decode(&buf[..n]).unwrap();
        assert!(header.encrypted);
        assert_eq!(header.codec, Codec::Opus, "codec still parsed under the flag");
    }

    #[test]
    fn roundtrip_pcm16() {
        roundtrip(Codec::Pcm16, 42, 1_234_567_890, SAMPLE_PAYLOAD);
    }

    #[test]
    fn roundtrip_opus() {
        roundtrip(Codec::Opus, u32::MAX, u64::MAX, SAMPLE_PAYLOAD);
    }

    #[test]
    fn roundtrip_empty_payload() {
        // A zero-length payload is legal framing (e.g. a keepalive).
        roundtrip(Codec::Pcm16, 0, 0, &[]);
    }

    #[test]
    fn magic_is_ascii_pm() {
        let mut buf = [0u8; 32];
        encode(Codec::Pcm16, false, 0, 0, SAMPLE_PAYLOAD, &mut buf).unwrap();
        // "PM" on the wire, little-endian.
        assert_eq!(&buf[0..2], b"PM");
    }

    #[test]
    fn reject_too_short() {
        assert_eq!(decode(&[]), Err(ProtocolError::TooShort));
        assert_eq!(decode(&[0u8; HEADER_LEN - 1]), Err(ProtocolError::TooShort));
    }

    #[test]
    fn reject_bad_magic() {
        let mut buf = [0u8; 32];
        let n = encode(Codec::Pcm16, false, 1, 1, SAMPLE_PAYLOAD, &mut buf).unwrap();
        buf[0] ^= 0xFF; // corrupt magic
        match decode(&buf[..n]) {
            Err(ProtocolError::BadMagic(_)) => {}
            other => panic!("expected BadMagic, got {other:?}"),
        }
    }

    #[test]
    fn reject_unsupported_version() {
        let mut buf = [0u8; 32];
        let n = encode(Codec::Pcm16, false, 1, 1, SAMPLE_PAYLOAD, &mut buf).unwrap();
        buf[2] = PROTOCOL_VERSION.wrapping_add(1);
        assert_eq!(
            decode(&buf[..n]),
            Err(ProtocolError::UnsupportedVersion(PROTOCOL_VERSION + 1))
        );
    }

    #[test]
    fn reject_unknown_codec() {
        let mut buf = [0u8; 32];
        let n = encode(Codec::Pcm16, false, 1, 1, SAMPLE_PAYLOAD, &mut buf).unwrap();
        buf[3] = 5; // not a known codec (low 7 bits; bit 7 is the encrypted flag)
        assert_eq!(decode(&buf[..n]), Err(ProtocolError::UnknownCodec(5)));
    }

    #[test]
    fn reject_truncated_payload() {
        // Header claims 10 payload bytes but the datagram carries only 5.
        let mut buf = [0u8; 32];
        let n = encode(Codec::Pcm16, false, 1, 1, SAMPLE_PAYLOAD, &mut buf).unwrap();
        let truncated = &buf[..n - 5];
        match decode(truncated) {
            Err(ProtocolError::PayloadLenMismatch { declared, actual }) => {
                assert_eq!(declared, 10);
                assert_eq!(actual, 5);
            }
            other => panic!("expected PayloadLenMismatch, got {other:?}"),
        }
    }

    #[test]
    fn reject_trailing_garbage() {
        // Extra bytes beyond the declared payload also fail, rather than being
        // silently accepted as a valid packet.
        let mut buf = [0u8; 64];
        let n = encode(Codec::Pcm16, false, 1, 1, SAMPLE_PAYLOAD, &mut buf).unwrap();
        let with_garbage = &buf[..n + 3];
        match decode(with_garbage) {
            Err(ProtocolError::PayloadLenMismatch { declared, actual }) => {
                assert_eq!(declared, 10);
                assert_eq!(actual, 13);
            }
            other => panic!("expected PayloadLenMismatch, got {other:?}"),
        }
    }

    #[test]
    fn encode_rejects_small_output() {
        let mut small = [0u8; HEADER_LEN + 2];
        let err = encode(Codec::Pcm16, false, 0, 0, SAMPLE_PAYLOAD, &mut small).unwrap_err();
        assert_eq!(
            err,
            ProtocolError::OutputTooSmall {
                needed: HEADER_LEN + SAMPLE_PAYLOAD.len(),
                got: HEADER_LEN + 2,
            }
        );
    }

    #[test]
    fn pcm16_sample_count_checks_alignment() {
        assert_eq!(pcm16_sample_count(&[0, 0, 0, 0]), Some(2));
        assert_eq!(pcm16_sample_count(&[]), Some(0));
        assert_eq!(pcm16_sample_count(&[0, 0, 0]), None);
    }

    #[test]
    fn header_len_constant_matches_layout() {
        // Guards against someone changing a field width without updating the
        // constant that the whole receiver relies on.
        assert_eq!(HEADER_LEN, 2 + 1 + 1 + 4 + 8 + 2);
    }
}
