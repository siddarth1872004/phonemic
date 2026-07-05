# PhoneMic

Use your Android phone's microphone as a real Windows microphone input — usable
by any Windows app (Discord, Zoom, OBS, Teams, …) through an actual kernel-mode
virtual audio driver, not a workaround. Three interchangeable transports:
same-Wi-Fi, USB, and Bluetooth. Built for low latency and low overhead: native
code on the entire audio hot path, no Electron, no JVM/CLR touching an audio
buffer.

**Platforms:** Android (phone) + Windows 11 (PC + driver). No macOS/Linux yet.

**Status:** the **web path works today** — a phone browser streams its mic to
the PC over HTTPS/WebSocket, through the jitter buffer, out the speakers
(verified end-to-end). The native Android path and the virtual driver are
written and waiting on toolchains/hardware. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full status table.

## Two ways in

- **Web (works now):** phone opens a web page — no install. See
  [docs/WEB-CLIENT.md](docs/WEB-CLIENT.md).
- **Native (lowest latency, USB/BT):** the Oboe + Kotlin app. Needs the Android
  toolchain; see [docs/PHASE0-BRINGUP.md](docs/PHASE0-BRINGUP.md).

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

## Quick start

```sh
# Rust core: framing + jitter-buffer tests (25 total), all green
cargo test

# Web path (needs w64devkit on PATH for `as`; see docs/BUILDING.md):
cargo run -p phonemic-web-gateway   # https://<pc-ip>:8443  → open on phone

# PC loopback demo without any phone:
cargo run -p phonemic-core          # udp :4010 → speakers
# in another shell, the software phone:
cargo run -p phonemic-core --bin softphone
```

Building the native Android app needs the Android SDK/NDK/CMake/JDK 17 — see
[docs/BUILDING.md](docs/BUILDING.md) and [docs/PHASE0-BRINGUP.md](docs/PHASE0-BRINGUP.md).

## License

Dual-licensed under either of [MIT](LICENSE-MIT) or
[Apache-2.0](LICENSE-APACHE) at your option.
