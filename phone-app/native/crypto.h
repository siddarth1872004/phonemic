// PhoneMic payload encryption (phone side) — mirrors pc-app/core/src/crypto.rs.
// XChaCha20-Poly1305 via monocypher, BLAKE2b-256 key derivation from a PIN.
// Verified byte-for-byte against the Rust decoder in native/test/crypto_test.c.
#pragma once

#include <stdint.h>
#include <string.h>

#include "monocypher.h"

#define PM_TAG_LEN 16
#define PM_KDF_SALT "phonemic/v1/kdf"  // 15 bytes, must match crypto.rs

// key = BLAKE2b-256(KDF_SALT || pin)
static inline void pm_derive_key(const char* pin, uint8_t key[32]) {
    uint8_t msg[15 + 64];
    size_t pl = strlen(pin);
    if (pl > 64) pl = 64;
    memcpy(msg, PM_KDF_SALT, 15);
    memcpy(msg + 15, pin, pl);
    crypto_blake2b(key, 32, msg, 15 + pl);
}

// nonce = seq(4 LE) || timestamp_us(8 LE) || 0*12
static inline void pm_nonce(uint32_t seq, uint64_t ts, uint8_t nonce[24]) {
    memset(nonce, 0, 24);
    for (int i = 0; i < 4; ++i) nonce[i] = (uint8_t)((seq >> (8 * i)) & 0xFF);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)((ts >> (8 * i)) & 0xFF);
}

// Encrypt `plaintext` (text_size bytes) into `out` as ciphertext||mac.
// `out` must hold text_size + PM_TAG_LEN bytes. `aad` is the packet header.
static inline void pm_encrypt(const uint8_t key[32],
                              const uint8_t* aad, size_t ad_size,
                              uint32_t seq, uint64_t ts,
                              const uint8_t* plaintext, size_t text_size,
                              uint8_t* out) {
    uint8_t nonce[24];
    pm_nonce(seq, ts, nonce);
    crypto_aead_lock(out, out + text_size, key, nonce, aad, ad_size, plaintext, text_size);
}
