#include <phonemic/crypto.h>
#include <phonemic/wire.h>

#include <string>
#include <vector>

#include "test.hpp"

namespace {
std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}
}  // namespace

// Known-answer vectors generated with independent implementations:
// argon2-cffi (reference Argon2) and libsodium's XChaCha20-Poly1305 (PyNaCl).
TEST(crypto_kdf_matches_reference_argon2id) {
    uint8_t key[32];
    const std::string pass = "correct horse";
    CHECK_EQ(pm_derive_key(pass.data(), pass.size(), 0x11223344u, key), 0);
    CHECK(hex(key, 32) == "2480b66976d364e268ca757ec46b1756a98a572fda4e064dcf5aa30f68d2a01b");
}

TEST(crypto_seal_matches_libsodium_xchacha20poly1305) {
    uint8_t key[32];
    const std::string pass = "correct horse";
    pm_derive_key(pass.data(), pass.size(), 0x11223344u, key);
    uint8_t aad[28];
    for (int i = 0; i < 28; ++i) aad[i] = static_cast<uint8_t>(i);
    const std::string pt = "PhoneMic v2 KAT!";
    std::vector<uint8_t> out(pt.size() + PM_TAG_LEN);
    pm_seal(key, aad, sizeof aad, 0x11223344u, 7, 0x0102030405060708ull,
            reinterpret_cast<const uint8_t*>(pt.data()), pt.size(), out.data());
    CHECK(hex(out.data(), out.size()) == "987d6f3a5a6b3506b5096392564246d29ce1b2b302f266d76a69a654d87e3782");
}

TEST(crypto_round_trip_and_auth_failures) {
    uint8_t key[32], other[32];
    pm_derive_key("123456", 6, 5, key);
    pm_derive_key("123457", 6, 5, other);
    uint8_t aad[28] = {0};
    const uint8_t pt[] = "forty-eight kilohertz mono pcm";
    std::vector<uint8_t> ct(sizeof pt + PM_TAG_LEN), back(sizeof pt);
    pm_seal(key, aad, sizeof aad, 5, 7, 99, pt, sizeof pt, ct.data());
    CHECK_EQ(pm_open(key, aad, sizeof aad, 5, 7, 99, ct.data(), ct.size(), back.data()), 0);
    CHECK(std::equal(back.begin(), back.end(), pt));
    // Wrong PIN.
    CHECK_EQ(pm_open(other, aad, sizeof aad, 5, 7, 99, ct.data(), ct.size(), back.data()), -1);
    // Tampered header (AAD).
    aad[3] = 1;
    CHECK_EQ(pm_open(key, aad, sizeof aad, 5, 7, 99, ct.data(), ct.size(), back.data()), -1);
    aad[3] = 0;
    // Replayed under a different seq (nonce) fails.
    CHECK_EQ(pm_open(key, aad, sizeof aad, 5, 8, 99, ct.data(), ct.size(), back.data()), -1);
    // Truncated.
    CHECK_EQ(pm_open(key, aad, sizeof aad, 5, 7, 99, ct.data(), 10, back.data()), -1);
}

TEST(crypto_key_differs_per_session) {
    uint8_t a[32], b[32];
    pm_derive_key("pin", 3, 1, a);
    pm_derive_key("pin", 3, 2, b);
    CHECK(hex(a, 32) != hex(b, 32));
}

TEST(crypto_nonce_layout) {
    uint8_t n[24];
    pm_nonce(0x04030201u, 0x08070605u, 0x100F0E0D0C0B0A09ull, n);
    for (int i = 0; i < 16; ++i) CHECK_EQ(n[i], i + 1);
    for (int i = 16; i < 24; ++i) CHECK_EQ(n[i], 0);
}
