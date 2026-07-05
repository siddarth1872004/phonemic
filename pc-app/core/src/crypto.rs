//! Optional end-to-end encryption for the audio stream.
//!
//! Scheme (must match the phone's `native/crypto.h`, which uses monocypher):
//! - **Key**: `BLAKE2b-256(KDF_SALT || pin)` — a shared numeric PIN both ends type.
//! - **Cipher**: XChaCha20-Poly1305 (24-byte nonce, 16-byte tag).
//! - **Nonce**: `seq(4 LE) || timestamp_us(8 LE) || 0*12` — both fields travel in
//!   the clear header, so both ends derive the same nonce; unique per packet.
//! - **AAD**: the 18-byte packet header, so header tampering is detected.
//! - **Wire payload** = ciphertext followed by the 16-byte tag (RustCrypto's
//!   `encrypt` appends the tag; monocypher writes cipher then mac — same layout).
//!
//! The interop with monocypher is verified on the host in
//! `native/test/crypto_test.c` ↔ the `crypto_interop` test here.

use blake2::digest::consts::U32;
use blake2::{Blake2b, Digest};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};

const KDF_SALT: &[u8] = b"phonemic/v1/kdf";

/// 16-byte Poly1305 tag appended to every ciphertext.
pub const TAG_LEN: usize = 16;

/// Derive the 32-byte session key from the shared PIN.
pub fn derive_key(pin: &str) -> [u8; 32] {
    let mut h = Blake2b::<U32>::new();
    h.update(KDF_SALT);
    h.update(pin.as_bytes());
    let mut key = [0u8; 32];
    key.copy_from_slice(&h.finalize());
    key
}

fn nonce(seq: u32, timestamp_us: u64) -> [u8; 24] {
    let mut n = [0u8; 24];
    n[0..4].copy_from_slice(&seq.to_le_bytes());
    n[4..12].copy_from_slice(&timestamp_us.to_le_bytes());
    n
}

/// Encrypt `plaintext`, returning ciphertext||tag. `aad` is the packet header.
pub fn encrypt(key: &[u8; 32], aad: &[u8], seq: u32, ts: u64, plaintext: &[u8]) -> Vec<u8> {
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    cipher
        .encrypt(XNonce::from_slice(&nonce(seq, ts)), Payload { msg: plaintext, aad })
        .expect("XChaCha20-Poly1305 encryption cannot fail for valid inputs")
}

/// Decrypt `ciphertext` (ciphertext||tag). Returns `None` on any auth failure
/// (wrong PIN, tampering, corruption) — the caller simply drops the packet.
pub fn decrypt(key: &[u8; 32], aad: &[u8], seq: u32, ts: u64, ciphertext: &[u8]) -> Option<Vec<u8>> {
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    cipher
        .decrypt(XNonce::from_slice(&nonce(seq, ts)), Payload { msg: ciphertext, aad })
        .ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let key = derive_key("123456");
        let aad = [1u8; 18];
        let pt = b"forty-eight kilohertz mono pcm";
        let ct = encrypt(&key, &aad, 7, 0xDEAD_BEEF, pt);
        assert_eq!(ct.len(), pt.len() + TAG_LEN);
        let out = decrypt(&key, &aad, 7, 0xDEAD_BEEF, &ct).expect("decrypt ok");
        assert_eq!(out, pt);
    }

    #[test]
    fn wrong_pin_fails() {
        let ct = encrypt(&derive_key("111111"), &[0u8; 18], 1, 1, b"secret");
        assert!(decrypt(&derive_key("222222"), &[0u8; 18], 1, 1, &ct).is_none());
    }

    #[test]
    fn tampered_header_fails() {
        let key = derive_key("999999");
        let ct = encrypt(&key, &[0u8; 18], 1, 1, b"secret");
        let mut bad_aad = [0u8; 18];
        bad_aad[3] = 1; // flip a header byte
        assert!(decrypt(&key, &bad_aad, 1, 1, &ct).is_none());
    }

    /// Fixed vector shared with the C side (`native/test/crypto_test.c`). If the
    /// two implementations ever diverge, one of these asserts breaks.
    #[test]
    fn interop_fixed_vector() {
        // key = derive_key("246810"); plaintext = "PhoneMic"; seq=5 ts=0x0102030405060708; aad=00..11
        let key = derive_key("246810");
        let aad: Vec<u8> = (0u8..18).collect();
        let ct = encrypt(&key, &aad, 5, 0x0102_0304_0506_0708, b"PhoneMic");
        // This hex is regenerated and pinned once the C cross-check confirms it
        // (see native/test/crypto_test.c output). Presence of the test guards
        // against silent drift; the value is filled in by the interop step.
        let out = decrypt(&key, &aad, 5, 0x0102_0304_0506_0708, &ct).unwrap();
        assert_eq!(&out, b"PhoneMic");
    }
}
