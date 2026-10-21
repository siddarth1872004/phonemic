# PhoneMic

PhoneMic turns your phone into a microphone for your computer. Speak into the
phone and the audio arrives on the PC as a normal microphone that Discord,
Zoom, Teams, OBS, browsers and every other app can select.

It is free and open source, needs no extra hardware or account, and is native
code throughout: a C++ desktop app for Windows and Linux, and an Android
app with a C++ audio engine. The phone reaches the PC over **Bluetooth**,
**Wi-Fi** or a **USB cable**, whichever is handy. Both apps are small and fast,
with a true-black (OLED) look by default, plus Dark and Light themes and a
choice of accent colours.

## Features

**Connecting**
- **Bluetooth:** pair the phone with the PC once, then pick the PC in the phone
  app. It uses an RFCOMM channel published through the system's own Bluetooth
  stack (BlueZ on Linux, the Windows Bluetooth stack); nothing extra is
  installed for it.
- **Wi-Fi:** the phone finds PhoneMic on the local network by itself (or you
  type the PC's address). Audio travels as UDP datagrams, the lowest-latency
  option.
- **USB:** with USB debugging on, PhoneMic on the PC runs `adb reverse` for
  every plugged-in phone, so the cable works with no setup and no network.
- The phone reconnects by itself if the link drops or the PC app restarts.
- The PC reports its state back to the phone (connected, PC busy, PIN wrong),
  so problems are shown where you are looking.

**Audio**
- Low latency: 10 ms packets, a lock-free audio path and a selectable buffer
  from 20 to 150 ms.
- Opus compression (about 48 kbps, comfortably inside a Bluetooth link) with
  forward error correction that rebuilds lost packets.
- Clock-drift compensation, so the delay does not creep up and there are no
  periodic clicks during long calls.
- Voice Focus noise removal (the phone's own suppressor plus RNNoise on the PC),
  a voice changer, gain with a limiter that cannot clip, and mute from either
  device.
- "Listen on this PC" lets you monitor yourself on any output device.

**The microphone**
- **Windows:** PhoneMic uses VB-CABLE, a free and signed virtual audio cable
  from VB-Audio, and installs it for you with one click (it downloads the
  official package and checks its VB-Audio signature first). PhoneMic plays
  into the cable automatically and keeps it from becoming your default
  speaker, which would send every PC sound into the microphone. One more click
  renames the cable's devices to **PhoneMic Microphone** and **PhoneMic Feed**.
- **Linux:** PhoneMic creates **PhoneMic Microphone** itself through PipeWire or
  PulseAudio, and can keep it registered when PhoneMic is closed.
- One click makes it the system's default microphone, so apps that have no
  microphone setting use it too.
- PhoneMic installs no driver of its own.

**Security**
- Bluetooth: only paired devices can connect, and Bluetooth encrypts the link.
- USB: the PC listens on 127.0.0.1 only, reachable through the cable alone.
- Wi-Fi: set a PIN on the PC and every audio packet must be sealed with it
  (Argon2id key, XChaCha20-Poly1305), so nobody else on the network can talk
  into your microphone or listen in.
- Only one phone can feed the microphone at a time.

**Look**
- OLED black (default), Dark and Light themes and five accent colours, chosen
  the same way on the phone and the PC.

## Download

Get the latest version from the
[Releases page](https://github.com/siddarth1872004/phonemic/releases).

| Platform | File |
|---|---|
| Windows 10 or 11 | `PhoneMic-windows-x64.zip` |
| Linux, any distribution | `PhoneMic-x86_64.AppImage` |
| Debian, Ubuntu, Mint | `phonemic_<version>_amd64.deb` |
| Fedora, openSUSE, RHEL 8+ | `phonemic-<version>-1.x86_64.rpm` |
| Linux, other | `PhoneMic-linux-x86_64.tar.gz` |
| Android 10 or newer | `PhoneMic.apk` |

## Getting started

### 1. Install PhoneMic on the computer

- **Windows:** unzip `PhoneMic-windows-x64.zip` and run `PhoneMic.exe`.
- **Linux:** make the AppImage executable and run it, or install the `.deb` or
  `.rpm` package.

### 2. Set up the microphone (once)

| System | What to do |
|---|---|
| Windows | Click **Install VB-CABLE** in the PhoneMic window, accept Windows' prompt, then restart Windows. Optionally click **Rename to PhoneMic** afterwards. |
| Linux | Nothing. PhoneMic creates the microphone automatically through PipeWire or PulseAudio. |

### 3. Connect the phone

Install `PhoneMic.apk`, choose a link at the top of the phone app, then tap
**Start**. It keeps streaming with the screen off.

| Link | Setup |
|---|---|
| Bluetooth | Pair the phone with the PC once in Bluetooth settings, then tap **Choose your PC** and pick it. |
| Wi-Fi | Phone and PC on the same network. Tap **Find your PC**. If you set a PIN in the PC's Wi-Fi section, enter the same PIN on the phone. |
| USB | Turn on USB debugging on the phone (Developer options), enable USB in the PhoneMic window, and plug in the cable. PhoneMic finds `adb` in the Android SDK or on the PATH. |

### 4. Choose the microphone in your apps

Select the microphone in the app's audio settings: **PhoneMic Microphone** on
Linux; on Windows **CABLE Output (VB-Audio Virtual Cable)**, or **PhoneMic
Microphone (VB-Audio Virtual Cable)** once renamed. For apps without a
microphone setting, click **Make it the default microphone** in the PhoneMic
window.

[START-HERE.md](START-HERE.md) has the same steps in more detail, with a
troubleshooting table.

## Requirements

| | Minimum |
|---|---|
| Windows | Windows 10 or 11, 64-bit. |
| Linux | glibc 2.28 or newer (Debian 10, Ubuntu 18.10, RHEL 8 and later), X11 or Wayland, and PipeWire or PulseAudio. Plain ALSA systems can use the `snd-aloop` loopback module instead. |
| Android | Android 10 or newer. |
| Bluetooth (optional) | Any built-in or USB Bluetooth adapter the system already supports. On Linux, BlueZ (the standard Bluetooth service). |
| USB (optional) | `adb` (Android SDK platform-tools) on the PC. |

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
Studio or Gradle. See
[docs/BUILDING.md](docs/BUILDING.md) for every platform, the build options and
how the release packages are made.

## How it works

```
 Phone                                          Computer
 +------------------------------+  Bluetooth    +-----------------------------------------+
 | microphone -> lock-free ring | ------------> | RFCOMM "PhoneMic" / UDP 4010 / TCP 4010 |
 | 10 ms frames, Opus           |  Wi-Fi (UDP)  | -> jitter buffer -> Opus / PCM decode,  |
 | (sealed with the PIN on      |  USB (adb)    |    loss concealment                     |
 |  Wi-Fi) -> link socket       | <------------ | -> noise removal -> effects -> limiter  |
 +------------------------------+  status 2 Hz  | -> drift-compensated playout            |
                                                | -> VB-CABLE / PhoneMic Microphone       |
                                                +-----------------------------------------+
```

The audio path is native on both ends, and its realtime parts never lock,
allocate memory or make system calls. On Android, Kotlin handles only the
interface and the app lifecycle.

The wire protocol lives in one C header
([`wire.h`](shared/include/phonemic/wire.h)) that is compiled unchanged into
both the phone and the desktop app, so the two cannot drift apart. The protocol
is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Each operating system gets its own microphone implementation:

| System | Implementation |
|---|---|
| Windows | VB-CABLE. The app plays into the cable's input; apps record its output. The endpoints can be renamed (as administrator, through the Windows audio property store); the driver itself is VB-Audio's and keeps its name. |
| Linux | A virtual sink and source created in PipeWire or PulseAudio through libpulse, which is loaded at run time. |

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the design in detail.

## Command-line tools

The desktop download also includes:

| Tool | Purpose |
|---|---|
| `phonemic-receiver` | The receiving engine without a window, for servers and scripts. `--make-default` makes PhoneMic the default microphone; on Linux, `--keep-mic on` keeps it registered when PhoneMic is closed. Run with `--help` for all options. |
| `phonemic-vmic-check` | Plays a test tone into the PhoneMic microphone and checks that it comes back out. `--setup --default` also checks the system default microphone. |

## Repository layout

| Path | Contents |
|---|---|
| `shared/` | The wire protocol and PIN crypto (C99 headers, Monocypher), used by the phone and the PC. |
| `pc-app/core/` | Desktop engine: Bluetooth, Wi-Fi and USB links, receive pipeline, buffers, DSP, audio devices, virtual microphone. |
| `pc-app/gui/` | The desktop app's window (Dear ImGui and GLFW). |
| `pc-app/tools/` | The command-line tools. |
| `pc-app/tests/` | Unit and integration tests. |
| `phone-app/` | The Android app (Kotlin interface, C++ and Oboe audio engine). |
| `packaging/` | The icon master (`linux/phonemic.svg`) and `make-icons.sh`, the desktop entry, AppStream metadata and the AppImage script. |
| `docs/` | Protocol, architecture, building, testing, release notes and the audit of the 0.2 codebase. |

## Testing

```sh
ctest --test-dir build --output-on-failure
```

The test suite covers the protocol (including fuzzing), PIN encryption, the
Bluetooth/USB stream framing, Wi-Fi discovery and PIN handling, the buffers under simulated clock drift, and
end-to-end transfers from framed phone packets to the microphone.

Continuous integration builds and tests on Linux and Windows, runs the
suite under AddressSanitizer and UndefinedBehaviorSanitizer, builds the Android
app, and installs the Linux packages on Debian, Ubuntu,
Fedora, Arch Linux, openSUSE and Rocky Linux. On Linux it also plays
a tone into the real PhoneMic microphone and checks that it comes back out,
both by name and as the system default. See [docs/TESTING.md](docs/TESTING.md).

## Known limitations

- Windows depends on VB-CABLE (free, by VB-Audio). Renaming changes the names
  apps show; the driver itself is VB-Audio's signed package and keeps its own
  name, which PhoneMic cannot change without breaking the signature.
- The VB-CABLE install, rename and default-speaker fix are compiled in CI
  (MSVC) but have no automated tests and have not yet been tried on a wide
  range of Windows machines. Reports are welcome.
- Linux distributions without glibc, such as Alpine, need a source build.
- The phone does not appear as a Bluetooth *headset* (HFP): Android apps
  cannot act as one. It appears to the PC as the **PhoneMic Microphone** device
  instead, which every app can use and which sounds far better than headset
  audio (48 kHz Opus instead of 16 kHz).
- Bluetooth adds roughly 20 to 40 ms over USB or Wi-Fi; the PC's buffer
  setting absorbs its jitter.
- iPhone is not supported.

## Documentation

- [START-HERE.md](START-HERE.md): step-by-step setup and troubleshooting
- [CHANGELOG.md](CHANGELOG.md) and [docs/releases/](docs/releases/): release history
- [docs/BUILDING.md](docs/BUILDING.md): building on every platform
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): design and components
- [docs/PROTOCOL.md](docs/PROTOCOL.md): the wire protocol
- [docs/TESTING.md](docs/TESTING.md): automated and manual testing
- [docs/AUDIT.md](docs/AUDIT.md): problems found in 0.2 and how they were fixed

## License

PhoneMic is dual-licensed under the [MIT License](LICENSE-MIT) and the
[Apache License 2.0](LICENSE-APACHE); you may use it under either.

Third-party components fetched at build time keep their own licenses:
RNNoise (BSD-3-Clause), Opus (BSD-3-Clause), Monocypher (CC0 or
BSD-2-Clause, vendored in `shared/third_party/`), miniaudio (MIT-0 or public
domain), Dear ImGui (MIT) and GLFW (zlib). VB-CABLE is VB-Audio's product
and is downloaded from vb-audio.com, not shipped with PhoneMic.
