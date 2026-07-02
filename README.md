# PhoneMic

Use your Android phone's microphone as a real Windows microphone input — usable
by any Windows app (Discord, Zoom, OBS, Teams, …) through an actual kernel-mode
virtual audio driver, not a workaround. Three interchangeable transports:
same-Wi-Fi, USB, and Bluetooth. Built for low latency and low overhead: native
code on the entire audio hot path, no Electron, no JVM/CLR touching an audio
buffer.

**Platforms:** Android (phone) + Windows 11 (PC + driver). No macOS/Linux yet.

**Status:** Phase 0 (loopback proof of life). See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the roadmap.

## Layout

```
/protocol       shared no_std Rust crate: packet framing, encode/decode (tested)
/pc-app/core    Rust: transports, jitter buffer, decode, dev-mode cpal playback
/pc-app/driver  C (WDK/ACX) virtual audio driver — Phase 4
/pc-app/ipc     core ↔ driver interface — Phase 4
/phone-app/app  Kotlin shell: UI, permissions, foreground service
/phone-app/native  C++: Oboe capture, libopus encode, JNI bridge (the hot path)
/docs           PROTOCOL.md, ARCHITECTURE.md, BUILDING.md
```

## Quick start (PC receiver, works today)

```sh
cargo test -p phonemic-protocol     # 13 passing framing tests
cargo run  -p phonemic-core         # listens on udp :4010, plays to speakers
```

Building the phone app needs the Android SDK/NDK/CMake/JDK 17 — see
[docs/BUILDING.md](docs/BUILDING.md).

## License

Dual-licensed under either of [MIT](LICENSE-MIT) or
[Apache-2.0](LICENSE-APACHE) at your option.
