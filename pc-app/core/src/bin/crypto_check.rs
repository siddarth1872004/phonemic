//! Cross-check the phone-side crypto (monocypher) against the Rust crypto:
//! read ciphertext||tag from stdin (produced by native/test/crypto_test.c using
//! the same fixed vector) and decrypt it with `phonemic_core::crypto`. Exit 0 on
//! a match. Build with `--features crypto`.

#[cfg(feature = "crypto")]
fn main() {
    use std::io::Read;

    let mut buf = Vec::new();
    std::io::stdin().read_to_end(&mut buf).expect("read stdin");

    let key = phonemic_core::crypto::derive_key("246810");
    let aad: Vec<u8> = (0u8..18).collect();
    match phonemic_core::crypto::decrypt(&key, &aad, 5, 0x0102_0304_0506_0708, &buf) {
        Some(pt) => {
            println!("decrypted: {:?}", String::from_utf8_lossy(&pt));
            assert_eq!(&pt, b"PhoneMic", "plaintext mismatch");
            println!("crypto interop PASSED (monocypher C ↔ Rust XChaCha20-Poly1305)");
        }
        None => {
            eprintln!("decrypt FAILED — phone and PC crypto do not interoperate");
            std::process::exit(1);
        }
    }
}

#[cfg(not(feature = "crypto"))]
fn main() {
    eprintln!("build with --features crypto");
    std::process::exit(2);
}
