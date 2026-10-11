# PhoneMic

PhoneMic turns your phone into a wireless microphone for your computer. Speak
into the phone and the audio arrives on the PC as a normal microphone,
**PhoneMic Microphone**, that Discord, Zoom, Teams, OBS, browsers and every
other app can select.

It is free and open source, needs no extra hardware or account, and is native
code throughout: a C++ desktop app for Windows, macOS and Linux, and an Android
app with a C++ audio engine. Any phone can also connect through its web browser
without installing anything.

## Features

**Connecting**
- The Android app finds the PC on the local network by itself; no IP addresses
  to type.
- Browser mode: scan a QR code shown by the PC and the phone's browser becomes
  the microphone (Android and iPhone).
- USB cable mode for poor or blocked Wi-Fi: PhoneMic sets up the connection
  over the cable automatically.
- The PC reports its state back to the phone (connected, wrong PIN, PC busy,
  not running), so problems are shown where you are looking.

**Audio**
- Low latency: 10 ms packets, a lock-free audio path and a selectable buffer
  from 20 to 150 ms.
- Opus compression (about 48 kbps instead of 768 kbps) with forward error
  correction that rebuilds lost packets.
- Clock-drift compensation, so the delay does not creep up and there are no
  periodic clicks during long calls.
- Voice Focus noise removal (the phone's own suppressor plus RNNoise on the PC),
  a voice changer, gain with a limiter that cannot clip, and mute from either
  device.
- "Listen on this PC" lets you monitor yourself on any output device.

**The microphone**
- PhoneMic installs its own microphone on every operating system. No
  third-party virtual audio cable is used or needed.
- One click makes it the system's default microphone, so apps that have no
  microphone setting use it too.
- On Linux it can stay registered when PhoneMic is closed, like a hardware
  microphone.

**Security**
- Optional encryption: the same PIN or passphrase on both devices gives
  XChaCha20-Poly1305 with Argon2id-derived keys. With a PIN set, unencrypted
  audio is refused.
- Only one phone can feed the microphone at a time.

## Download

Get the latest version from the
[Releases page](https://github.com/siddarth1872004/phonemic/releases).

| Platform | File |
|---|---|
| Windows 11 | `PhoneMic-windows-x64.zip` (app and microphone driver) |
| macOS 11 or newer | `PhoneMic-macos.zip` (Apple silicon and Intel) |
| Linux, any distribution | `PhoneMic-x86_64.AppImage` |
| Debian, Ubuntu, Mint | `phonemic_<version>_amd64.deb` |
| Fedora, openSUSE, RHEL 8+ | `phonemic-<version>-1.x86_64.rpm` |
| Linux, other | `PhoneMic-linux-x86_64.tar.gz` |
| Android 10 or newer | `PhoneMic.apk` |

## Getting started

### 1. Install PhoneMic on the computer

- **Windows:** unzip `PhoneMic-windows-x64.zip` and run `PhoneMic.exe`. Keep the
  `driver` folder next to it. Allow PhoneMic through the firewall on private
  networks when Windows asks.
- **macOS:** unzip and open `PhoneMic.app`. The first time, right-click it and
  choose Open.
- **Linux:** make the AppImage executable and run it, or install the `.deb` or
  `.rpm` package.

### 2. Set up the microphone (once)

| System | What to do |
|---|---|
| Windows 11 | Click **Install PhoneMic driver** in the PhoneMic window, then restart when asked. |
| macOS | Click **Install PhoneMic driver** and enter your password. |
| Linux | Nothing. PhoneMic creates the microphone automatically through PipeWire or PulseAudio. |

On Windows the installer explains each step first. Until the driver is signed
by Microsoft, Windows only loads it in test mode, which requires Secure Boot to
be off. The installer asks before turning test mode on, and
`uninstall-driver.ps1` reverses everything.

### 3. Connect the phone

- **Android app:** install `PhoneMic.apk`, tap **Find my PC**, then **Start**.
  It keeps streaming with the screen off.
- **Any phone, no app:** tick **Browser mic** in the PhoneMic window, scan the QR
  code, accept the PC's own certificate once (Advanced, then Proceed) and tap
  **Start mic**.
- **USB cable:** enable USB debugging on the phone, tick **Stream over USB** in
  the PC window, plug the phone in and turn on **USB cable** in the phone app.

### 4. Choose the microphone in your apps

Select **PhoneMic Microphone** in the app's audio settings. Windows lists it as
**Microphone (PhoneMic Microphone)**. For apps without a microphone setting,
click **Make it the default microphone** in the PhoneMic window.

[START-HERE.md](START-HERE.md) has the same steps in more detail, with a
troubleshooting table.

## Requirements

| | Minimum |
|---|---|
| Windows | Windows 11, version 22H2 (build 22621). Windows 10 is not supported. |
| macOS | macOS 11. |
| Linux | glibc 2.28 or newer (Debian 10, Ubuntu 18.10, RHEL 8 and later), X11 or Wayland, and PipeWire or PulseAudio. Plain ALSA systems can use the `snd-aloop` loopback module instead. |
| Android | Android 10. |
| Browser mode | A current browser on the phone, on the same network as the PC. |

## Building from source

The desktop app uses CMake 3.21+ and a C++17 compiler. All third-party code is
fetched at pinned versions during configuration.

```sh
git clone https://github.com/siddarth1872004/phonemic
cd phonemic
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The binaries are placed in `build/bin/`. Linux needs the X11 and OpenGL
development headers. The Android app is built from `phone-app/` with Android
Studio or Gradle, and the Windows driver with the Windows Driver Kit. See
[docs/BUILDING.md](docs/BUILDING.md) for every platform, the build options and
how the release packages are made.

## How it works

```
 Phone                                          Computer
 +------------------------------+   Wi-Fi UDP   +-----------------------------------------+
 | microphone -> lock-free ring | ------------> | decode -> authenticate / decrypt        |
 | 10 ms frames, Opus, optional |   or USB      | -> jitter buffer -> Opus / PCM decode,  |
 | encryption                   | <------------ |    loss concealment                     |
 +------------------------------+  status 2 Hz  | -> noise removal -> effects -> limiter  |
 Phone browser -- HTTPS / WebSocket ----------> | -> drift-compensated playout            |
                                                | -> PhoneMic Microphone                  |
                                                +-----------------------------------------+
```

The audio path is native on both ends, and its realtime parts never lock,
allocate memory or make system calls. On Android, Kotlin handles only the
interface and the app lifecycle.

The wire protocol and the encryption live in two C headers
([`wire.h`](shared/include/phonemic/wire.h),
[`crypto.h`](shared/include/phonemic/crypto.h)) that are compiled unchanged into
both the phone and the desktop app, so the two cannot drift apart. The protocol
is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Each operating system gets its own microphone implementation:

| System | Implementation |
|---|---|
| Windows | A kernel-mode audio driver (WDF + ACX). The app writes audio into a shared-memory ring that the driver creates and reads on its own clock. |
| macOS | A Core Audio plug-in (`PhoneMic.driver`): a loopback device loaded by the system audio service, with no kernel extension. |
| Linux | A virtual sink and source created in PipeWire or PulseAudio through libpulse, which is loaded at run time. |

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the design in detail.

## Command-line tools

The desktop download also includes:

| Tool | Purpose |
|---|---|
| `phonemic-receiver` | The receiving engine without a window, for servers and scripts. `--make-default` makes PhoneMic the default microphone; on Linux, `--keep-mic on` keeps it registered when PhoneMic is closed. Run with `--help` for all options. |
| `phonemic-softphone` | Emulates a phone for testing, including discovery, encryption and simulated packet loss. |
| `phonemic-vmic-check` | Plays a test tone into the PhoneMic microphone and checks that it comes back out. `--setup --default` also checks the system default microphone. |

## Repository layout

| Path | Contents |
|---|---|
| `shared/` | Wire protocol and encryption (C99 headers and monocypher), used by the phone and the PC. |
| `pc-app/core/` | Desktop engine: receive pipeline, buffers, DSP, audio devices, virtual microphone, browser-mic server. |
| `pc-app/gui/` | The desktop app's window (Dear ImGui and GLFW). |
| `pc-app/tools/` | The command-line tools. |
| `pc-app/tests/` | Unit and integration tests. |
| `pc-app/driver/`, `pc-app/ipc/` | The Windows microphone driver and the shared ring the app feeds it through. |
| `pc-app/driver-macos/` | The macOS microphone driver. |
| `phone-app/` | The Android app (Kotlin interface, C++ and Oboe audio engine). |
| `web-client/` | The browser microphone page, embedded in the desktop app at build time. |
| `packaging/linux/` | Desktop entry, icon, AppStream metadata and the AppImage script. |
| `docs/` | Protocol, architecture, building, testing, release notes and the audit of the 0.2 codebase. |

## Testing

```sh
ctest --test-dir build --output-on-failure
```

The test suite covers the protocol (including fuzzing), the encryption
(against known-answer vectors from independent implementations), the buffers
under simulated clock drift, and end-to-end transfers over UDP, TLS and USB.

Continuous integration builds and tests on Linux, Windows and macOS, runs the
suite under AddressSanitizer and UndefinedBehaviorSanitizer, builds the Android
app and the Windows driver, and installs the Linux packages on Debian, Ubuntu,
Fedora, Arch Linux, openSUSE and Rocky Linux. On Linux and macOS it also plays
a tone into the real PhoneMic microphone and checks that it comes back out,
both by name and as the system default. See [docs/TESTING.md](docs/TESTING.md).

## Known limitations

- The Windows driver needs test mode until it is attestation-signed by
  Microsoft, which requires an EV code-signing certificate and a Partner Center
  account. Everything else is prepared; see [docs/SIGNING.md](docs/SIGNING.md).
- The Windows driver is built and packaged in CI but has not yet been tested on
  a wide range of hardware. Reports are welcome.
- Linux distributions without glibc, such as Alpine, need a source build.
- There is no Bluetooth transport. USB mode covers the case where Wi-Fi is
  unavailable, with lower latency than Bluetooth.

## Documentation

- [START-HERE.md](START-HERE.md): step-by-step setup and troubleshooting
- [CHANGELOG.md](CHANGELOG.md) and [docs/releases/](docs/releases/): release history
- [docs/BUILDING.md](docs/BUILDING.md): building on every platform
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): design and components
- [docs/PROTOCOL.md](docs/PROTOCOL.md): the wire protocol
- [docs/TESTING.md](docs/TESTING.md): automated and manual testing
- [docs/SIGNING.md](docs/SIGNING.md): getting the Windows driver signed by Microsoft
- [docs/AUDIT.md](docs/AUDIT.md): problems found in 0.2 and how they were fixed
- [pc-app/driver/README.md](pc-app/driver/README.md) and
  [pc-app/driver-macos/README.md](pc-app/driver-macos/README.md): the microphone drivers

## License

PhoneMic is dual-licensed under the [MIT License](LICENSE-MIT) and the
[Apache License 2.0](LICENSE-APACHE); you may use it under either.

Third-party components fetched at build time keep their own licenses:
monocypher (BSD-2-Clause or CC0), RNNoise (BSD-3-Clause), Opus (BSD-3-Clause),
miniaudio (MIT-0 or public domain), Dear ImGui (MIT), GLFW (zlib), Mbed TLS
(Apache-2.0) and QR Code generator (MIT).
