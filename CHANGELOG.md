# Changelog

## 0.3.1 — 2026-10

Full notes: [docs/releases/v0.3.1.md](docs/releases/v0.3.1.md).

### Changed
- PhoneMic uses only its own microphone, "PhoneMic Microphone". The VB-CABLE
  installer and the VB-CABLE / BlackHole fallbacks are gone.

### Added
- **Make it the default microphone** (Windows, macOS, Linux), so apps without a
  microphone picker use PhoneMic too; `phonemic-receiver --make-default`.
- Linux: **Keep it when PhoneMic is closed** (PipeWire / PulseAudio
  configuration), so apps opened earlier list it; `phonemic-receiver --keep-mic`.
- `phonemic-vmic-check --default` and, on Windows, a check through the driver.
- CI records the system default microphone on Linux (PulseAudio and PipeWire)
  and macOS, and checks the Linux microphone survives a sound-server restart.

### Fixed
- Windows driver: explicit capture categories and an always-connected jack, so
  Windows creates a visible endpoint instead of possibly none or an "unplugged" one.

## 0.3.0 — 2026-10

Full notes: [docs/releases/v0.3.0.md](docs/releases/v0.3.0.md). Bug list with
causes: [docs/AUDIT.md](docs/AUDIT.md).

### Added
- PhoneMic's own virtual microphone, "PhoneMic Microphone", on every OS:
  Windows ACX driver (with installer, local test-signing and uninstaller),
  macOS Core Audio plug-in, Linux PipeWire/PulseAudio via libpulse (no pactl),
  ALSA `snd-aloop` fallback. VB-CABLE and BlackHole are only fallbacks now.
- Linux packages for every glibc distro: AppImage, .deb, .rpm, tarball;
  native Wayland as well as X11; desktop entry, icon, AppStream metadata.
- macOS release (universal app with the driver inside).
- Opus compression with in-band FEC and packet-loss concealment.
- USB cable mode (`adb reverse`, set up automatically).
- LAN discovery ("Find my PC") and PC → phone status feedback.
- Browser microphone built into the desktop app (HTTPS, QR code, pairing code).
- Adaptive playout with clock-drift compensation.
- `phonemic-vmic-check`: plays a tone into the virtual mic and checks it comes back.
- CI: Linux, Windows, macOS, ASan/UBSan, Android, the Windows driver (WDK),
  and package installs on Debian, Ubuntu, Fedora, Arch, openSUSE and Rocky Linux 8.

### Changed
- The desktop app is rewritten in C++17 (Dear ImGui + GLFW + miniaudio); one
  protocol and crypto implementation (`shared/`) is compiled into both the phone
  and the PC.

### Fixed
- About 60 bugs; see docs/AUDIT.md.

## 0.2.0

Windows desktop app (Rust) with VB-CABLE, Android app (Kotlin + C++/Oboe),
Voice Focus, voice changer, PIN encryption.
