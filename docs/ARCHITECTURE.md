# PhoneMic architecture

Turn an Android phone's microphone into a real Windows microphone input, usable
by any Windows app, over Wi-Fi / USB / Bluetooth, at low latency.

## Guiding principles

1. **Hot path is native.** Audio capture, encode, decode, playback never touch a
   JVM, CLR, or webview. Kotlin/C# are for UI, permissions, and lifecycle only.
2. **Latency is a first-class requirement.** Targets: < ~50 ms end-to-end on
   Wi-Fi/USB, < ~100 ms on Bluetooth. Buffer sizes, codec frame size, and
   transport choices are all made against these numbers.
3. **Minimal, justified dependencies.**
4. **A real kernel driver**, not a shell-out to a third-party tool.
5. **Test the loop before the hard parts** — audible phone→PC first.

## Data flow (end state)

```
 Phone (native hot path)                    PC
 ┌───────────────────────────┐             ┌────────────────────────────────────┐
 │ Oboe capture (AAudio)     │             │ transport recv                     │
 │   → libopus encode        │  packets    │   → jitter buffer (reorder/smooth) │
 │   → protocol::encode      │ ──────────► │   → libopus decode (+ PLC)         │
 │   → UDP / RFCOMM send     │  (Wi-Fi/    │   → shared-memory ring             │
 └───────────────────────────┘   USB/BT)   │   → ACX virtual driver             │
   Kotlin: UI, permissions,                │        → Windows capture endpoint  │
   foreground service, transport pick      └────────────────────────────────────┘
                                             Rust core (net→decode); C driver
```

The `phonemic-protocol` crate is the single source of truth for the packet
format, compiled natively for the PC and cross-compiled into the phone's native
layer (via cbindgen/JNI) so there is exactly one framing implementation.

## Components

| Path | Language | Role |
|---|---|---|
| `/protocol` | Rust (`no_std`) | Packet framing, encode/decode, version negotiation. Tested. |
| `/pc-app/core` | Rust | Transport trait + impls, jitter buffer, Opus decode, dev-mode `cpal` sink. |
| `/pc-app/driver` | C (WDK/ACX) | Virtual audio driver exposing a capture endpoint. Phase 4. |
| `/pc-app/ipc` | — | Core↔driver interface (shared-memory ring). Phase 4. |
| `/phone-app/native` | C++ | Oboe capture, libopus encode, JNI bridge. The hot path. |
| `/phone-app/app` | Kotlin | UI, permissions, foreground service, transport selection. |

## Transports (all behind the `Transport` trait)

- **Wi-Fi:** raw UDP, custom framing (Phases 0–1). mDNS/NSD discovery in Phase 1.
- **USB:** `adb forward` tunnel reusing the Wi-Fi protocol (Phase 2). Raw USB
  accessory mode only if explicitly requested. Note: the `adb` tunnel is TCP, so
  its impl must add length-prefix framing to preserve message boundaries.
- **Bluetooth:** RFCOMM/SPP, same framing (Phase 3). Expect Windows API quirks.

## Build phases

- **Phase 0 — Loopback proof of life (current).** Android Oboe capture → PCM16
  over UDP → Rust receiver → `cpal` speaker playback. Hear your own voice.
- **Phase 1 — Wi-Fi hardened.** Opus + PLC, real jitter buffer, mDNS discovery,
  measured latency.
- **Phase 2 — USB.** `adb forward` tunnel.
- **Phase 3 — Bluetooth.** RFCOMM.
- **Phase 4 — Windows driver.** ACX driver (structured after `sysvad`) + shared
  memory IPC. Requires WDK + test-signing; production signing is a later concern.

## Status

Phase 0 scaffolded. Rust `/protocol` (13 passing tests) and `/pc-app/core`
receiver build and run on this machine. The Android half is scaffolded but
**not yet buildable here** — it needs Android SDK + NDK + CMake + JDK 17+
installed (see `BUILDING.md`). Audible end-to-end loop is pending that toolchain.
