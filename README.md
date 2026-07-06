<div align="center">

# 🎙️ PhoneMic

**Turn your Android phone into a real wireless microphone for your Windows or Linux PC.**

No hardware. No subscriptions. Native code end-to-end — Rust + C++ on the hot path, zero Electron.

![Platform](https://img.shields.io/badge/PC-Windows%20%7C%20Linux-0078D4?logo=windows&logoColor=white)
![Platform](https://img.shields.io/badge/Phone-Android%2010%2B-3DDC84?logo=android&logoColor=white)
![CI](https://github.com/siddarth1872004/phonemic/actions/workflows/build.yml/badge.svg)
![Rust](https://img.shields.io/badge/PC%20app-Rust-orange?logo=rust)
![C++](https://img.shields.io/badge/Audio%20engine-C%2B%2B%20%2F%20Oboe-blue)
![License](https://img.shields.io/badge/license-MIT%20%2F%20Apache--2.0-green)

*Speak into your phone → it shows up as a microphone in Discord, Zoom, OBS, Teams — any desktop app.*

</div>

---

## ✨ Features

| | |
|---|---|
| 🚀 **Low latency** | Raw UDP + 10 ms audio frames + lock-free buffers. Tuned end-to-end for real-time voice. |
| 🎭 **Voice changer** | Live presets — **Deep**, **Chipmunk**, **Robot** — switchable mid-call. Hand-written DSP, pitch-shift verified by unit tests. |
| 🧹 **Voice Focus** | Two-stage noise removal: the phone's hardware noise suppressor + echo canceller, then RNNoise on the PC. Kills fans, keyboards, room hiss. |
| 🔒 **Encrypted option** | Type the same PIN on both ends → XChaCha20-Poly1305 end-to-end encryption. Leave it blank for zero-config LAN use. |
| 📵 **Runs in background** | Foreground service on the phone — lock the screen, keep talking. |
| 🎚️ **Live level meters** | On both the phone and the PC, so you always know audio is flowing. |
| 🛠️ **One-click mic setup** | The PC app downloads and launches the VB-CABLE virtual-microphone installer for you. |
| 🩺 **Self-diagnosing** | The status line tells you *why* when something isn't connected (wrong PIN, stale app, network path) instead of leaving you guessing. |

## 🚀 Quick start

### 1 · PC

Grab a prebuilt binary from [**Releases**](https://github.com/siddarth1872004/phonemic/releases) (`PhoneMic.exe` for Windows, `PhoneMic-linux` for Linux), or build it:

```sh
git clone https://github.com/siddarth1872004/phonemic
cd phonemic
# Linux: sudo apt install libasound2-dev   (Debian/Ubuntu; alsa-lib-devel on Fedora, alsa-lib on Arch)
cargo run --release -p phonemic-gui       # Windows needs MinGW binutils — see docs/BUILDING.md
```
The window shows **your PC's IP address**.

### 2 · Phone
Build `phone-app/` with Android Studio (open → Run ▶), or install a prebuilt `PhoneMic.apk` if you have one. Open the app, type the IP from the PC window, tap **Start**, allow the microphone.

### 3 · Make it a real microphone

**Windows** — click **⚙ Set up microphone** in the app (one-time, installs the free VB-CABLE driver, reboot), then in Discord / Zoom / OBS pick:
> 🎙 **CABLE Output (VB-Audio Virtual Cable)**

**Linux** — nothing to install: PhoneMic creates a PulseAudio/PipeWire virtual source automatically at startup (needs `pactl`, preinstalled on most distros). Pick:
> 🎙 **PhoneMic Microphone**

Done. Your phone is now your mic — Voice Focus, voice changer and all.

## 🏗️ How it works

```
 📱 Android phone                             🖥️ Windows PC
 ┌───────────────────────────┐               ┌──────────────────────────────────┐
 │ Oboe capture (C++, 48 kHz)│               │ UDP receive (Rust)               │
 │  → hardware noise/echo fx │   Wi-Fi UDP   │  → decrypt (XChaCha20, optional) │
 │  → XChaCha20 encrypt (opt)│ ────────────► │  → RNNoise Voice Focus (opt)     │
 │  → 18-byte framed packets │  10ms frames  │  → voice changer DSP (opt)       │
 │    (Kotlin = UI only)     │               │  → VB-CABLE ➜ appears as a mic   │
 └───────────────────────────┘               └──────────────────────────────────┘
```

**Principle #1: the hot path is native.** Audio buffers are only ever touched by C++ (phone) and Rust (PC). Kotlin does UI, permissions and lifecycle — never samples.

The wire protocol is a single `no_std` Rust crate ([`protocol/`](protocol/)) with the C mirror ([`wire.h`](phone-app/native/wire.h)) **cross-checked byte-for-byte against it in CI-able host tests** — same for the encryption (monocypher ↔ RustCrypto interop test). See [docs/PROTOCOL.md](docs/PROTOCOL.md).

## 📁 Repository layout

| Path | What it is |
|---|---|
| [`protocol/`](protocol/) | Shared `no_std` wire format — framing, versioning, encrypted flag. 14 tests. |
| [`pc-app/gui/`](pc-app/gui/) | **The desktop app** — Windows + Linux (Rust + egui, single small binary). |
| [`pc-app/core/`](pc-app/core/) | Transports, jitter buffer, RNNoise, voice-changer DSP, crypto. 30+ tests. |
| [`pc-app/web-gateway/`](pc-app/web-gateway/) | Optional browser-based phone client (no APK needed). |
| [`pc-app/driver/`](pc-app/driver/) + [`ipc/`](pc-app/ipc/) | Future native ACX virtual-mic driver (code-complete, needs WDK). |
| [`phone-app/`](phone-app/) | **The Android app** — Kotlin UI + C++/Oboe audio engine. |
| [`docs/`](docs/) | Protocol spec, architecture, build guides. |

## 🧪 Tests & verification

```sh
cargo test            # protocol framing, jitter buffer, DSP, crypto — all green
```
Cross-implementation checks run on the host, no device needed:
- C++ `wire.h` framing → decoded by the Rust `protocol` crate ✅
- monocypher (phone crypto) → decrypted by RustCrypto (PC crypto) ✅
- Pitch shifter output frequency measured against target ratios ✅

## 🗺️ Roadmap

- [x] Wi-Fi transport, framed UDP, jitter buffer
- [x] Voice Focus (RNNoise + Android hardware fx)
- [x] Voice changer (pitch / robot)
- [x] Optional end-to-end encryption (PIN)
- [x] Browser client fallback (web gateway)
- [x] Linux support (PulseAudio/PipeWire virtual mic — no driver needed)
- [x] CI builds for Windows + Linux on every push
- [ ] Opus codec (packet-loss concealment, lower bandwidth)
- [ ] USB transport (`adb` tunnel) · Bluetooth RFCOMM
- [ ] Native ACX virtual-mic driver (drop VB-CABLE; WDK required)
- [ ] mDNS auto-discovery (no IP typing at all)

## 📄 License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE) — pick whichever suits you.

---

<div align="center">
<sub>Built with Rust 🦀, C++ ⚙️ and an unreasonable dislike of Electron.</sub>
</div>
