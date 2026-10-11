# PhoneMic virtual microphone driver for macOS

`PhoneMic.driver` publishes **PhoneMic Microphone**, a loopback audio device, so
PhoneMic doesn't need BlackHole or any other third-party driver. It is a Core
Audio **AudioServerPlugIn**: user-space code loaded by `coreaudiod`, no kernel
extension, no reduced security settings. Written from scratch against Apple's
public `AudioServerPlugIn.h` API (no code from other virtual-audio projects).

```
PhoneMic app ──plays into──► output side ─┐  ring indexed by sample time
Zoom / Discord / OBS ◄─records─ input side ◄┘  (one device, one clock)
```

- 2-channel 32-bit float, 44.1 or 48 kHz.
- The output side can't become the system's default output, so speaker audio
  never leaks into the mic; the input side can be the default microphone.
- Each ring frame is tagged with the sample time it was written for; the input
  side plays a frame only if the tag matches, so stale audio is never heard.
- Both sides run in the same IO cycle on the device's clock; PhoneMic's adaptive
  playout already matches the phone's clock to it.

## Install

In the app: **Install PhoneMic driver** (asks for your password). Or:

```sh
sudo PhoneMic.app/Contents/Resources/install-driver.sh            # install / update
sudo PhoneMic.app/Contents/Resources/install-driver.sh --uninstall
```

It copies the bundle to `/Library/Audio/Plug-Ins/HAL/` and restarts Core Audio.

## Build

CMake builds it on macOS (`PhoneMicAudio` target, `build/bin/PhoneMic.driver`),
signs it ad hoc and copies it into `PhoneMic.app/Contents/Resources`. CI
installs it on the macOS runner and runs `phonemic-vmic-check --setup`, which
plays a tone into the device and checks it comes back out.
