# Building PhoneMic

## PC side (Rust) — works today

Requires a Rust toolchain (tested with 1.96). From the repo root:

```sh
# Run the protocol unit tests
cargo test -p phonemic-protocol

# Build the Phase 0 receiver
cargo build -p phonemic-core --release

# Run it (listens on udp 0.0.0.0:4010 by default; override with PHONEMIC_PORT)
cargo run -p phonemic-core
# or the built binary:
target/release/phonemic-receiver.exe
```

### Testing the PC pipeline without a phone

`softphone` emulates the Android sender on the PC using the real wire protocol,
so the whole receive path can be exercised and latency-measured with no device:

```sh
cargo build -p phonemic-core --bins --release

# terminal A: receiver in latency-measurement mode
PHONEMIC_PORT=4067 target/release/phonemic-receiver.exe --measure

# terminal B: stream synthetic audio at it
target/release/softphone.exe 127.0.0.1 4067
```

The receiver collects 300 packets and prints one-way latency (min/median/p95/max
+ jitter). On loopback this isolates software/OS overhead (sub-millisecond); a
real phone adds Oboe capture + Wi-Fi, measured with the clap test in
[PHASE0-BRINGUP.md](PHASE0-BRINGUP.md).

The receiver prints the address it's listening on. Point the phone app at this
PC's LAN IP and port 4010.

> For the full step-by-step to finish Phase 0 (install → build → run → measure
> latency), see **[PHASE0-BRINGUP.md](PHASE0-BRINGUP.md)**. The summary below is
> just the prerequisite list.

## Phone side (Android) — prerequisites not yet installed on this machine

The native hot path (Oboe + libopus + JNI) and the Kotlin shell need:

- **Android Studio** (or the standalone SDK command-line tools)
- **Android SDK Platform 34** and **build-tools**
- **Android NDK** (r26+) — compiles the C++ capture/encode code
- **CMake 3.22+** (installable via the SDK Manager)
- **JDK 17+** — the Java on this machine is 1.8, which modern Android Gradle
  rejects. Install a newer JDK (Android Studio bundles one).
- **`adb`** on `PATH` — to deploy to the phone and for the Phase 2 USB tunnel.

Once installed, create `phone-app/local.properties` with `sdk.dir=<path>` (or set
`ANDROID_HOME`), then:

```sh
cd phone-app
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

### Native dependencies (fetched by the CMake build, not vendored)

- **Oboe** — Google's low-latency audio library. Add via the `oboe` Prefab AAR
  dependency, or `FetchContent` in CMake.
- **libopus** — added in Phase 1. Phase 0 sends raw PCM16 and needs no codec.

## Windows driver (Phase 4) — not started

Will require the Windows Driver Kit (WDK) matching the installed Visual Studio,
plus test-signing enabled (`bcdedit /set testsigning on`) on the dev machine.
Production driver signing (EV cert / attestation signing) is a separate,
later concern and is out of scope until the driver works under test-signing.
