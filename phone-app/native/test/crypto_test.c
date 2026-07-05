// Host cross-check: encrypt a fixed vector with the phone-side crypto (monocypher)
// and write ciphertext||mac to stdout. The Rust `crypto_check` bin decrypts it
// and asserts the plaintext — proving the phone and PC crypto interoperate,
// without a device.
//
//   gcc -I.. crypto_test.c ../monocypher.c -o crypto_test
//   ./crypto_test | cargo run -q -p phonemic-core --features crypto --bin crypto_check
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../crypto.h"

int main(void) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    uint8_t key[32];
    pm_derive_key("246810", key);

    uint8_t aad[18];
    for (int i = 0; i < 18; ++i) aad[i] = (uint8_t)i;

    const char* pt = "PhoneMic";
    const size_t n = 8;
    uint8_t out[8 + PM_TAG_LEN];
    pm_encrypt(key, aad, 18, 5, 0x0102030405060708ULL, (const uint8_t*)pt, n, out);

    fwrite(out, 1, n + PM_TAG_LEN, stdout);
    return 0;
}
