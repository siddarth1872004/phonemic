<div align="center">

# 🎙️ PhoneMic

**Turn your phone into a real wireless microphone for your PC.**

No extra hardware, no subscription, no Electron. Native code end to end: a C++ desktop app and a C++/Oboe Android engine that share one protocol implementation.

![Platform](https://img.shields.io/badge/PC-Windows%20%7C%20Linux%20%7C%20macOS*-0078D4)
![Platform](https://img.shields.io/badge/Phone-Android%2010%2B%20%7C%20any%20browser-3DDC84)
![CI](https://github.com/siddarth1872004/phonemic/actions/workflows/build.yml/badge.svg)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus)
![License](https://img.shields.io/badge/license-MIT%20%2F%20Apache--2.0-green)

*Speak into your phone and it shows up as a microphone in Discord, Zoom, OBS, Teams, or any other desktop app.*

</div>

---

## ✨ Features

| | |
|---|---|
| 🔎 **Zero typing** | The phone app finds your PC on the Wi-Fi by itself ("Find my PC"). |
| 🌐 **No app needed** | Or scan the QR code in the PC window: the phone's **browser** becomes the mic (Android and iPhone). |
| 📦 **Opus compression** | ~48 kbps instead of 768 kbps raw PCM, with forward error correction that rebuilds lost packets. On by default; one switch in the app. |
| 🚀 **Low latency** | 10 ms UDP frames, a lock-free audio path, a Wi-Fi low-latency lock on the phone, and a selectable buffer (20–150 ms). |
| 🎚️ **Glitch-free over time** | Adaptive playout absorbs phone/PC clock drift (no creeping delay, no periodic clicks). The jitter buffer handles reordering and loss concealment. |
| 🧹 **Voice Focus** | The phone's noise suppressor and echo canceller, then RNNoise on the PC. Removes fans, keyboards and room hiss. |
| 🎭 **Voice changer** | Deep, Chipmunk, Robot, Radio, Echo; switch mid-call. |
| 🔊 **Volume and mute** | Gain up to +18 dB with a limiter that can't clip, plus mute from the PC, the phone, or the phone's notification. |
| 🔒 **Encryption** | Same PIN or passphrase on both ends gives XChaCha20-Poly1305 with Argon2id keys. With a PIN set, unauthenticated audio is refused. |
| 🩺 **Tells you what's wrong** | The PC reports back to the phone: *connected / wrong PIN / PIN required / PC busy / not running / no network*. |
| 🎧 **Hear yourself** | "Listen on this PC" monitors on any output while the mic keeps feeding your apps. |
| 🛠️ **One-click mic setup** | Windows: downloads and signature-checks VB-CABLE. Linux: creates a PulseAudio/PipeWire virtual mic automatically. |

## 🚀 Quick start

### 1 · PC

Download from [**Releases**](https://github.com/siddarth1872004/phonemic/releases): `PhoneMic.exe` (Windows) or `PhoneMic-linux-x64.tar.gz`. Or build it:

```sh
git clone https://github.com/siddarth1872004/phonemic && cd phonemic
cmake --preset release && cmake --build --preset release     # see docs/BUILDING.md
./build/bin/PhoneMic                                          # Windows: build\bin\Release\PhoneMic.exe
```

### 2 · Phone (pick one)

- **Android app** (best latency, works with the screen off): install `PhoneMic.apk` from Releases, or build `phone-app/` in Android Studio. Tap **🔍 Find my PC**, then **Start**.
- **Any phone's browser**: tick **Browser mic** in the PC window, scan the QR code, then tap *Advanced → Proceed* once (it's the PC's own certificate) and **Start mic**.

### 3 · Use it as a microphone

| OS | One-time setup | Pick this mic in your apps |
|---|---|---|
| **Windows** | Click **Set up microphone (VB-CABLE)** in the app, then reboot | `CABLE Output (VB-Audio Virtual Cable)` |
| **Linux** | Nothing (needs `pactl`, which PulseAudio and PipeWire systems have) | `PhoneMic Microphone` |
| **macOS*** | `brew install blackhole-2ch` | `BlackHole 2ch` |

<sub>* macOS builds in CI but is experimental.</sub>

## 🏗️ How it works

```
 📱 Android (C++ / Oboe)                          🖥️ PhoneMic desktop app (C++)
 ┌──────────────────────────────┐   Wi-Fi UDP   ┌───────────────────────────────────────────┐
 │ mic → lock-free ring          │ ────────────► │ decode (shared wire.h) → auth / decrypt   │
 │ sender thread: 10 ms frames,  │  10 ms frames │ → jitter buffer → Opus/PCM → FEC, PLC     │
 │ wire.h framing, XChaCha20 opt │ ◄──────────── │ → RNNoise → voice FX → gain/limiter       │
 └──────────────────────────────┘  STATUS 2 Hz  │ → adaptive playout (drift) → virtual mic  │
 🌐 any browser ── HTTPS/WebSocket (pairing code) ─►  same pipeline                         │
                                                 └───────────────────────────────────────────┘
```

**The hot path is native.** Kotlin does the UI, permissions and lifecycle; it never touches audio. The realtime callbacks on both ends never lock, allocate, or make system calls.

**One protocol implementation.** [`shared/include/phonemic/wire.h`](shared/include/phonemic/wire.h) and [`crypto.h`](shared/include/phonemic/crypto.h) are compiled unchanged into the Android engine *and* the desktop app. See [docs/PROTOCOL.md](docs/PROTOCOL.md).

## 📁 Repository layout

| Path | What it is |
|---|---|
| [`shared/`](shared/) | The wire protocol and encryption (C99 headers + monocypher), used by phone and PC. |
| [`pc-app/core/`](pc-app/core/) | Desktop engine: receiver pipeline, jitter buffer, adaptive playout, RNNoise, voice FX, audio devices, virtual mic, browser-mic server. |
| [`pc-app/gui/`](pc-app/gui/) | **The desktop app**: Dear ImGui + GLFW, one small native binary. |
| [`pc-app/tools/`](pc-app/tools/) | `phonemic-receiver` (headless) and `phonemic-softphone` (phone emulator for testing). |
| [`pc-app/tests/`](pc-app/tests/) | 92 unit and integration tests (protocol fuzzing, crypto known-answer vectors, drift simulation, end-to-end UDP and TLS). |
| [`pc-app/ipc/`](pc-app/ipc/) + [`pc-app/driver/`](pc-app/driver/) | Experimental ACX virtual-mic driver to replace VB-CABLE (needs the WDK). |
| [`phone-app/`](phone-app/) | **The Android app**: Kotlin UI + C++/Oboe engine. |
| [`web-client/`](web-client/) | The browser mic page (embedded into the desktop app at build time). |
| [`docs/`](docs/) | Protocol, architecture, building, testing, and the [v0.2 → v0.3 audit](docs/AUDIT.md). |

## 🧪 Tests

```sh
ctest --test-dir build --output-on-failure
```

CI runs the suite on Linux, Windows (MSVC) and macOS, plus an ASan/UBSan build, and builds the Android app with lint. Highlights:

- the protocol decoder is fuzzed and never reads out of bounds;
- crypto is pinned to known-answer vectors from *independent* implementations (reference Argon2, libsodium);
- the playout buffer holds latency against ±300 ppm clock drift over 3 simulated minutes;
- end-to-end tests over real UDP (discovery, status feedback, PIN rejection) and real TLS (page, pairing code, WebSocket audio).

The browser client was also checked against the real server in headless Chromium with a fake microphone.

## 🗺️ Roadmap

- [x] Wi-Fi UDP transport, jitter buffer, loss concealment, drift compensation
- [x] Voice Focus, voice changer, gain/limiter, mute
- [x] Encryption (Argon2id + XChaCha20-Poly1305), sender lock
- [x] LAN auto-discovery and PC → phone status feedback
- [x] Browser mic built into the app (QR code + pairing code)
- [x] Windows, Linux, macOS (experimental) desktop builds; Android in CI
- [x] Opus codec (48 kbps, FEC + concealment)
- [ ] USB (`adb reverse`) and Bluetooth transports
- [ ] Native ACX virtual-mic driver (code written, needs WDK build + signing)

## 📄 License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE). Third-party components are fetched at build time under their own licenses: monocypher (BSD-2/CC0), RNNoise (BSD-3), miniaudio (MIT-0/public domain), Dear ImGui (MIT), GLFW (zlib), Mbed TLS (Apache-2.0), QR Code generator (MIT).
