//! Cross-implementation wire check: read a packet from stdin (produced by the
//! phone-side `native/test/wire_test.cpp` using `wire.h`) and decode it with the
//! real `phonemic_protocol` decoder, asserting the known fields. Exit 0 on a
//! byte-for-byte match, nonzero otherwise. This is how we guarantee the C++
//! sender and the Rust receiver agree without a device in the loop.

use std::io::Read;

use phonemic_protocol::{decode, Codec};

fn main() {
    let mut buf = Vec::new();
    std::io::stdin()
        .read_to_end(&mut buf)
        .expect("read stdin");

    match decode(&buf) {
        Ok((h, payload)) => {
            println!(
                "decoded: codec={:?} seq={} ts=0x{:016x} payload_len={} payload={:?}",
                h.codec, h.seq, h.timestamp_us, h.payload_len, payload
            );
            assert_eq!(h.codec, Codec::Pcm16, "codec mismatch");
            assert_eq!(h.seq, 12345, "seq mismatch");
            assert_eq!(h.timestamp_us, 0x1122334455667788, "timestamp mismatch");
            assert_eq!(payload, &[0, 1, 2, 3, 4, 5, 6, 7], "payload mismatch");
            println!("cross-impl wire check PASSED (C++ wire.h ↔ Rust decoder)");
        }
        Err(e) => {
            eprintln!("decode failed: {e:?}");
            std::process::exit(1);
        }
    }
}
