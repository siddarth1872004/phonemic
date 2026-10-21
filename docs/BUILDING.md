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
| `phonemic-vmic-check` | plays a tone into the virtual mic and checks it comes back (`--setup` uses the app's own detection; `--default` records the system default microphone) |
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
At run time the app talks to PipeWire or PulseAudio through libpulse, loaded on
demand (no build-time dependency), and falls back to ALSA. Bluetooth goes
through BlueZ over D-Bus, with libdbus also loaded on demand, so there is no
Bluetooth build dependency either. The default build
uses X11 (XWayland on Wayland desktops); `-DPHONEMIC_WAYLAND=ON` adds a native
Wayland window (needs `wayland-scanner`, libwayland and libxkbcommon headers).

**Packages for every distro** (what CI and releases do, in a glibc 2.28
container so the result runs on any newer distro):
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPHONEMIC_PORTABLE=ON -DPHONEMIC_WAYLAND=ON
cmake --build build --parallel
(cd build && cpack -G "DEB;RPM")                                  # .deb + .rpm
cmake --install build --prefix "$PWD/stage" --component phonemic
packaging/linux/build-appimage.sh "$PWD/stage" ./appimagetool     # AppImage (needs desktop-file-utils)
```
`PHONEMIC_PORTABLE` links libstdc++/libgcc statically. Distros without glibc
(Alpine, musl) build from source.

### Build options

| Option | Default | |
|---|---|---|
| `PHONEMIC_BUILD_GUI` | ON | the window (needs X11/GL headers on Linux) |
| `PHONEMIC_BUILD_TOOLS` | ON | receiver + vmic-check |
| `PHONEMIC_BUILD_TESTS` | ON | test suite |
| `PHONEMIC_WITH_RNNOISE` | ON | Voice Focus noise removal |
| `PHONEMIC_WAYLAND` | OFF | Linux: native Wayland window as well as X11 |
| `PHONEMIC_PORTABLE` | OFF | Linux: static libstdc++/libgcc (one binary for every distro) |

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

The app has one library dependency, Oboe (the native audio input); its UI uses
framework widgets only. The native engine (`phone-app/native`) compiles
`shared/` directly, so the protocol is always identical to the desktop app's.

## App icon
`packaging/linux/phonemic.svg` is the master. `packaging/make-icons.sh`
(needs `rsvg-convert` and Python 3) renders the PNG sizes and rebuilds
`pc-app/gui/phonemic.ico`, which `phonemic.rc` embeds in the Windows exe. The
Android launcher icon is vector drawables (`res/drawable/ic_launcher_*.xml`)
drawn from the same shape.
