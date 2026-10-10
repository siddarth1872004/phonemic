/*
 * PhoneMic payload encryption — shared by the phone and the PC (single source).
 *
 * Optional: when the user types the same PIN / passphrase on both ends, every
 * AUDIO payload is sealed with XChaCha20-Poly1305 (monocypher).
 *
 *   key   = Argon2id(passphrase, salt = "phonemic/v2/" || session_id LE,
 *                    4 MiB, 3 passes, 1 lane) -> 32 bytes
 *   nonce = session_id(4 LE) || seq(4 LE) || timestamp_us(8 LE) || 0 * 8
 *   aad   = the packet's 28-byte header (tampering with any field fails auth)
 *   wire  = ciphertext || 16-byte tag
 *
 * Why it changed from v1 (BLAKE2b(salt || pin), nonce = seq || ts):
 *  - v1 used one fast, unsalted-per-user hash, so a single captured packet let an
 *    attacker try all 10^6 six-digit PINs in well under a second. Argon2id with a
 *    per-session salt makes every guess cost ~4 MiB of memory-hard work and
 *    prevents precomputed tables. (A short numeric PIN is still guessable
 *    offline by a determined attacker — use a longer passphrase for real
 *    secrecy; the UI says so.)
 *  - v1 nonces could repeat across app restarts (seq restarts at 0 under the
 *    same static key). Now the key itself is per session, and the session id is
 *    also mixed into the nonce.
 */
#ifndef PHONEMIC_CRYPTO_H
#define PHONEMIC_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "monocypher.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_KEY_LEN        32
#define PM_TAG_LEN        16
#define PM_NONCE_LEN      24
#define PM_KDF_BLOCKS     4096u /* KiB of memory per derivation */
#define PM_KDF_PASSES     3u
#define PM_KDF_SALT_PREFIX "phonemic/v2/" /* 12 bytes; + 4-byte session id = 16 */
#define PM_MAX_PASSPHRASE 128

/* Derive the per-session key. Returns 0 on success, -1 if the 4 MiB work area
 * could not be allocated. Cost: a few tens of milliseconds; callers derive once
 * per session, never per packet. */
static inline int pm_derive_key(const char* pass, size_t pass_len, uint32_t session_id,
                                uint8_t key[PM_KEY_LEN]) {
    uint8_t salt[16];
    crypto_argon2_config cfg;
    crypto_argon2_inputs in;
    void* work = malloc((size_t)PM_KDF_BLOCKS * 1024u);
    if (!work) return -1;
    if (pass_len > PM_MAX_PASSPHRASE) pass_len = PM_MAX_PASSPHRASE;

    memcpy(salt, PM_KDF_SALT_PREFIX, 12);
    salt[12] = (uint8_t)session_id;
    salt[13] = (uint8_t)(session_id >> 8);
    salt[14] = (uint8_t)(session_id >> 16);
    salt[15] = (uint8_t)(session_id >> 24);

    cfg.algorithm = CRYPTO_ARGON2_ID;
    cfg.nb_blocks = PM_KDF_BLOCKS;
    cfg.nb_passes = PM_KDF_PASSES;
    cfg.nb_lanes = 1;
    in.pass = (const uint8_t*)pass;
    in.pass_size = (uint32_t)pass_len;
    in.salt = salt;
    in.salt_size = sizeof salt;

    crypto_argon2(key, PM_KEY_LEN, work, cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work, (size_t)PM_KDF_BLOCKS * 1024u);
    free(work);
    return 0;
}

static inline void pm_nonce(uint32_t session_id, uint32_t seq, uint64_t ts,
                            uint8_t nonce[PM_NONCE_LEN]) {
    int i;
    memset(nonce, 0, PM_NONCE_LEN);
    for (i = 0; i < 4; ++i) nonce[i] = (uint8_t)(session_id >> (8 * i));
    for (i = 0; i < 4; ++i) nonce[4 + i] = (uint8_t)(seq >> (8 * i));
    for (i = 0; i < 8; ++i) nonce[8 + i] = (uint8_t)(ts >> (8 * i));
}

/* Seal `text_size` bytes of `plain` into `out` (ciphertext || tag); `out` must
 * hold text_size + PM_TAG_LEN bytes and may alias `plain`. */
static inline void pm_seal(const uint8_t key[PM_KEY_LEN], const uint8_t* aad, size_t aad_size,
                           uint32_t session_id, uint32_t seq, uint64_t ts,
                           const uint8_t* plain, size_t text_size, uint8_t* out) {
    uint8_t nonce[PM_NONCE_LEN];
    pm_nonce(session_id, seq, ts, nonce);
    crypto_aead_lock(out, out + text_size, key, nonce, aad, aad_size, plain, text_size);
}

/* Open `sealed` (ciphertext || tag, `sealed_size` bytes) into `out`
 * (sealed_size - PM_TAG_LEN bytes). Returns 0 on success, -1 on any auth failure
 * (wrong passphrase, tampering, truncation); nothing is decrypted then. */
static inline int pm_open(const uint8_t key[PM_KEY_LEN], const uint8_t* aad, size_t aad_size,
                          uint32_t session_id, uint32_t seq, uint64_t ts,
                          const uint8_t* sealed, size_t sealed_size, uint8_t* out) {
    uint8_t nonce[PM_NONCE_LEN];
    size_t text_size;
    if (sealed_size < PM_TAG_LEN) return -1;
    text_size = sealed_size - PM_TAG_LEN;
    pm_nonce(session_id, seq, ts, nonce);
    return crypto_aead_unlock(out, sealed + text_size, key, nonce, aad, aad_size, sealed,
                              text_size) == 0
               ? 0
               : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* PHONEMIC_CRYPTO_H */
