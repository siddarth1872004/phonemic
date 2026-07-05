// Host test: encode a packet with wire.h (the phone-side framing) and write the
// raw bytes to stdout. Piped into `phonemic-wire-check` (the real Rust decoder),
// this proves the C++ sender and Rust receiver agree on the byte layout — no
// Android device required. Built with a host g++ (see run instructions below).
//
//   g++ -std=c++17 -I.. wire_test.cpp -o wire_test
//   ./wire_test | cargo run -q -p phonemic-core --bin phonemic-wire-check
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "wire.h"

int main() {
#ifdef _WIN32
    // Emit binary, not text, so no CRLF translation corrupts the packet.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Known fields the Rust checker asserts on.
    uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t out[64];
    int n = pm_encode(PM_CODEC_PCM16, /*encrypted*/ 0, /*seq*/ 12345u,
                      /*ts*/ 0x1122334455667788ULL, payload, 8, out);
    fwrite(out, 1, static_cast<size_t>(n), stdout);
    return 0;
}
