# Building PhoneMic

## Desktop app (C++17, CMake)

Everything third-party is fetched by CMake at configure time, at pinned
versions (`cmake/Dependencies.cmake`). You need git, CMake ≥ 3.21 and a C++17
compiler; on Linux also the X11/GL headers for the window.

```sh
cmake --preset release          # = cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build --preset release  # = cmake --build build --config Release --parallel
ctest --preset release          # run the tests
```

Binaries land in `build/bin/` (`build/bin/Release/` with Visual Studio):

| Binary | What |
|---|---|
| `PhoneMic` / `PhoneMic.exe` | the desktop app |
| `phonemic-receiver` | the same engine without a window (servers, scripting, `--measure`) |
| `phonemic-softphone` | emulates the phone: tone, PIN, loss/reorder simulation, discovery |
| `phonemic-tests` | the test suite |

### Windows
Install **Visual Studio 2022 Build Tools** (workload *Desktop development with C++*)
and **CMake**, then from a *Developer PowerShell*:
```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```
The executable is fully static (no VC++ redistributable needed). MinGW-w64 works too.

### Linux
```sh
# Debian/Ubuntu
sudo apt install build-essential cmake git libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev
# Fedora:  sudo dnf install gcc-c++ cmake git libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel
# Arch:    sudo pacman -S base-devel cmake git libx11 libxrandr libxinerama libxcursor libxi mesa
```
At runtime the app talks to PulseAudio or PipeWire (through `pactl` and libpulse,
loaded on demand) and falls back to ALSA. It runs on Wayland through XWayland.

### macOS (experimental)
Xcode command-line tools + CMake. Install BlackHole for the virtual mic.

### Build options

| Option | Default | |
|---|---|---|
| `PHONEMIC_BUILD_GUI` | ON | the window (needs X11/GL headers on Linux) |
| `PHONEMIC_BUILD_TOOLS` | ON | receiver + softphone |
| `PHONEMIC_BUILD_TESTS` | ON | test suite |
| `PHONEMIC_WITH_RNNOISE` | ON | Voice Focus noise removal |
| `PHONEMIC_WITH_WEB` | ON | built-in browser mic (mbedTLS, QR code) |

`cmake --preset headless` builds everything except the window.

## Android app

Requirements: **Android Studio** (it bundles JDK 17), and in its SDK Manager
**Android 14 (API 34)**, **NDK 26.1.10909125** and **CMake 3.22.1**.

- Android Studio: open `phone-app/` and press **Run ▶**.
- Command line:
  ```sh
  cd phone-app
  echo "sdk.dir=/path/to/Android/Sdk" > local.properties   # or set ANDROID_HOME
  ./gradlew assembleRelease        # app/build/outputs/apk/release/app-release.apk
  adb install -r app/build/outputs/apk/release/app-release.apk
  ```
  Release builds are minified and, unless you add your own signing config, signed
  with the debug key so they install directly.

The native engine (`phone-app/native`) compiles `shared/` directly, so protocol
and crypto are always identical to the desktop app's.

## Driver (experimental)
See [pc-app/driver/README.md](../pc-app/driver/README.md). It needs the WDK (or
the WDK NuGet package, as CI uses) and is not part of the CMake build.
