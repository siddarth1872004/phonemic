# Changelog

## 0.5.1 — 2026-11

Full notes: [docs/releases/v0.5.1.md](docs/releases/v0.5.1.md).

### Fixed
- Soundboard sounds were muffled: Opus ran in speech mode and the PC's Voice
  focus (RNNoise) treated clips as noise. While a sound plays the phone now
  encodes full-band at 96 kbps and flags those frames (`PM_FLAG_SOUND`), and
  the PC skips denoising for them. The PC app must be updated with the phone
  app: an older PC drops flagged frames.

### Changed
- Phone app: **Mute mic** and **Mute sounds** are separate. Mute (main screen,
  notification) now mutes only the mic; the Soundboard screen has both.
- New look for both apps, styled like studio hardware: graphite faceplates,
  LED lamps and keys, monospace readouts, and a round level meter. On the
  phone the meter ring is the Start/Stop key; on the PC its centre mutes. A
  tally light reads ON AIR / MUTED / STANDBY. Themes are now Black, Graphite
  and Silver; accents Signal (new default), Teal, Violet, Rose and Ice. Saved
  theme and accent choices carry over by position.

## 0.5.0 — 2026-11

Full notes: [docs/releases/v0.5.0.md](docs/releases/v0.5.0.md).

### Added
- Phone app: **Soundboard**. It opens the MyInstants site, and tapping a sound
  sends it into the PhoneMic microphone, mixed with your voice, instead of
  playing it on the phone's speaker. It has its own level control and a limiter
  so voice plus sound cannot clip. The PC app and the protocol are unchanged.

## 0.4.1 — 2026-10

Full notes: [docs/releases/v0.4.1.md](docs/releases/v0.4.1.md).

### Added
- Desktop app: closing the window hides PhoneMic to the system tray and the
  microphone keeps working; left-click the tray icon to show or hide the
  window, and use its menu to quit. Linux (StatusNotifierItem, e.g. KDE Plasma)
  and Windows. Without a tray, closing quits as before.
- Starting PhoneMic again shows the window that is already running instead of
  opening a second copy.
- Arch Linux: `packaging/arch/PKGBUILD` (`cd packaging/arch && makepkg -si`).

### Fixed
- Linux (Wayland): the window could freeze after being re-shown or minimised,
  waiting on a vsync frame callback that never came.

## 0.4.0 — 2026-10

Full notes: [docs/releases/v0.4.0.md](docs/releases/v0.4.0.md).

### Added
- Bluetooth: the phone streams to the paired PC over RFCOMM through the
  operating system's own Bluetooth stack (BlueZ over D-Bus, Winsock AF_BTH),
  alongside Wi-Fi and USB. The phone app picks the link.
- Windows: VB-CABLE integration. It is installed from the app (downloaded from
  vb-audio.com, Authenticode-checked), PhoneMic routes into it automatically,
  its endpoints can be renamed "PhoneMic Microphone" / "PhoneMic Feed", and the
  cable is kept from being the default speaker.
- Themes (OLED black, Dark, Light) and five accent colours on both apps.
- A new app icon: the Windows exe resource, the Linux desktop icon and an
  Android adaptive icon with a monochrome layer.

### Changed
- Windows 10 or 11, 64-bit; the Windows 11 22H2 minimum is gone.
- Android: framework widgets only (AndroidX and Material removed).
- Release builds drop unused code and symbols (`--gc-sections`, `-s`).
- Child processes (adb) no longer inherit PhoneMic's sockets.

### Removed
- PhoneMic's own Windows (ACX) microphone driver, its installer and the
  test-signing steps; VB-CABLE replaces it.
- macOS support: the macOS app, the `PhoneMic.driver` Core Audio plug-in and
  the macOS build. PhoneMic now runs on Windows and Linux.
- The browser microphone (HTTPS, WebSocket, QR code), `phonemic-softphone`,
  and the Mbed TLS and QR code dependencies.

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
