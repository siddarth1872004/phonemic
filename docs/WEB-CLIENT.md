# Web client (browser as the phone)

An alternative to the native Android app: the phone opens a **web page** that
captures its mic and streams to the PC. No install, no SDK/NDK, works on Android
*and* iOS. This is a **hybrid** with the native app, not a replacement — see the
tradeoffs below.

## How it works

```
 Phone browser                                   PC (phonemic-web-gateway)
 ┌──────────────────────────────┐               ┌────────────────────────────┐
 │ getUserMedia (mic)           │               │ HTTPS: serves web-client/  │
 │  → AudioWorklet (PCM16, 10ms)│  wss:// binary │ wss /ws: WebSocket         │
 │  → protocol::encode (JS)     │ ────────────► │  → protocol::decode         │
 │  → WebSocket.send            │  PCM16 frames  │  → cpal output (speakers)  │
 └──────────────────────────────┘               └────────────────────────────┘
```

The page is served **over HTTPS by the gateway** (browsers only expose
`getUserMedia` in a secure context). Because the page and the WebSocket share an
origin, `wss://…/ws` reuses the cert the browser already accepted for the page.
Frames use the **exact same `phonemic-protocol` framing** as the native app and
the `softphone` — one wire format everywhere.

## Why WebSocket + raw PCM16 (and not WebRTC)

| Option | Verdict |
|---|---|
| **WebSocket + PCM16** ✅ | Chosen. Universal browser support; reuses the protocol crate, jitter buffer, and cpal sink unchanged; no Opus, so no CMake dependency. On a LAN, TCP reliability costs little. |
| **WebRTC** | Best-in-theory for lossy links (Opus + PLC + jitter + echo-cancel, all native in the browser — actually honors principle #1). **But** WebRTC audio is always Opus, so the PC must *decode* Opus — which needs libopus/CMake, currently unavailable. Also a heavy `webrtc-rs` + signaling stack. Deferred, not rejected. |
| **WebTransport** | UDP-like datagrams (matches our loss-tolerant design) and could carry our PCM16 protocol directly. Good future upgrade; newer, thinner PC-side support. |

## Tradeoffs vs. the native app

- **Bends principle #1.** Capture goes through an `AudioWorklet` — JavaScript
  touches the PCM (on the audio thread, not main, but still JS/GC-capable). The
  native app keeps the hot path pure C++. The web client trades that for
  zero-install reach. The native path remains the pure-native option.
- **No USB, no Bluetooth.** Browsers can't do RFCOMM or USB-accessory. The web
  client is Wi-Fi/network only. USB/BT stay native-only (Phases 2–3).
- **48 kHz assumption.** We force `AudioContext({sampleRate: 48000})`; if a
  browser refuses, pitch shifts (the protocol has no sample-rate field yet).
- **Self-signed cert warning.** The user accepts it once per device.

Unchanged either way: the **Windows virtual driver** (Phase 4) is what makes
this show up as a real microphone; the transport doesn't affect it.

## Building & running the gateway

```sh
cargo run -p phonemic-web-gateway            # serves https://0.0.0.0:8443
# then on the phone: open https://<PC-LAN-IP>:8443/  → accept cert → Start mic
```

### Toolchain requirement (important)

The gateway needs TLS, which pulls `raw-dylib` crates (`getrandom`,
`windows-sys`). Building those on a **`x86_64-pc-windows-gnu`** toolchain
requires GNU binutils — specifically `dlltool` **and** `as`. The rustup gnu
toolchain bundles `dlltool` but **not** `as`, so on a bare gnu setup the build
fails with `dlltool ... CreateProcess`. Two fixes:

1. **Complete the GNU toolchain** — install MSYS2 / MinGW-w64, then put its
   `bin` (containing `as.exe`, `dlltool.exe`) on `PATH`.
2. **Switch to the MSVC toolchain** (no `dlltool` needed) — install the Visual
   Studio Build Tools (C++ + Windows SDK), then:
   ```sh
   rustup toolchain install stable-x86_64-pc-windows-msvc
   rustup default stable-x86_64-pc-windows-msvc
   ```

`protocol` and `pc-app/core` build fine on the bare gnu toolchain (they avoid
`raw-dylib`), which is why they remain the workspace's `default-members`.

On this dev machine the gateway builds by putting the already-installed
**w64devkit** (its `bin` folder, which has `as` + `dlltool`) on
`PATH` before `cargo build -p phonemic-web-gateway`. It has been built and
verified end-to-end there (served the page over HTTPS and streamed 200 PCM16
frames — including reordered and dropped frames — through the jitter buffer).

### Opus status (deferred)

CMake is installed, but the Rust libopus bindings tried (`audiopus_sys`,
`opusic-sys`) both hit Windows/MinGW build-script bugs (an extended-path `cp`
failure, and a CMake+Ninja compiler self-test failure with w64devkit's `ar`).
PCM16 works today, so Opus is deferred. Paths to enable it: build libopus once
by hand and link the `opus` crate via `pkg-config`, or build on an MSVC
toolchain where these `-sys` crates work out of the box. Opus's value (PLC, lower
bitrate) matters most for Bluetooth/internet, not the LAN web path.
