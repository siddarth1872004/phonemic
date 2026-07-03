# Phase 0 bring-up: from zero to an audible phone→PC loop + a latency number

This is the exact sequence to finish Phase 0 on a fresh Windows machine. Steps 1
and 3 are the only ones that need you (installs / physical setup); everything
else is a couple of commands.

---

## 1. Install the Android toolchain (one pass)

Install **Android Studio** — it bundles a JDK 17 (the JetBrains Runtime) and the
SDK Manager, which is the least-effort way to get everything below.

Then in **Android Studio → Settings → Languages & Frameworks → Android SDK**:

- **SDK Platforms** tab → check **Android 14 (API 34)**.
- **SDK Tools** tab → check "Show Package Details" and select exactly:
  | Component | Version |
  |---|---|
  | Android SDK Platform-Tools (gives `adb`) | latest |
  | Android SDK Build-Tools | 34.0.0 |
  | **NDK (Side by side)** | 26.1.10909125 (r26b) or newer |
  | **CMake** | 3.22.1 |

Apply / OK to download.

### Environment

- Set `ANDROID_HOME` to the SDK path (default
  `C:\Users\<you>\AppData\Local\Android\Sdk`).
- Add `%ANDROID_HOME%\platform-tools` to `PATH` so `adb` works in a terminal.
- For **Gradle from the command line** (not needed if you build inside Android
  Studio): set `JAVA_HOME` to a JDK 17. Android Studio ships one at
  `…\Android Studio\jbr`; or install Temurin 17.

Verify:

```powershell
adb version           # should print a version
java -version         # must be 17.x for CLI builds (Studio uses its own JBR)
```

---

## 2. Build and install the phone app

> **Gap to know about:** the repo does **not** contain the Gradle wrapper
> (`gradlew`, `gradle/wrapper/gradle-wrapper.jar`) — that jar is a binary and
> wasn't committed. Two ways to get it:
>
> - **Easiest:** open `phone-app/` in Android Studio. It syncs, generates the
>   wrapper, and downloads Gradle automatically. Then just press **Run**.
> - **CLI:** with a system Gradle installed, run once:
>   `cd phone-app && gradle wrapper --gradle-version 8.7`
>   (AGP 8.5 requires Gradle 8.7+.)

Point the build at your SDK — create `phone-app/local.properties`:

```
sdk.dir=C:\\Users\\<you>\\AppData\\Local\\Android\\Sdk
```

Then build a debug APK and install it on a USB-connected phone (with USB
debugging enabled in the phone's Developer Options):

```powershell
cd phone-app
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

First build is slow: it compiles the C++ (Oboe + capture) for each ABI via CMake.

---

## 3. Wire the two ends together

1. Put the **phone and PC on the same Wi-Fi** network.
2. Find the PC's LAN IP: `ipconfig` → the IPv4 address of your Wi-Fi adapter
   (e.g. `192.168.1.23`).
3. **Open UDP 4010 on the Windows firewall** (the receiver binds it). In an
   **admin** PowerShell:
   ```powershell
   netsh advfirewall firewall add rule name="PhoneMic UDP 4010" `
     dir=in action=allow protocol=UDP localport=4010
   ```
   (Or accept the firewall pop-up the first time the receiver runs.)

---

## 4. Run the loop

Terminal on the PC:

```powershell
cargo run -p phonemic-core --release
```

It prints the address it's listening on and the output device. On the phone:
enter the PC IP, tap **Start mic**, grant the microphone permission. Speak — you
should hear yourself from the PC speakers, delayed only by network + buffering.

Sanity checks if you hear nothing:

| Symptom | Likely cause |
|---|---|
| Receiver never prints "first packet from …" | firewall, wrong IP, or different Wi-Fi networks (or client isolation on the AP) |
| "first packet" prints but silence | PC output muted / wrong default device; check the printed device line |
| "dropped malformed packet" spam | version/format mismatch — protocol drift between phone and PC |
| Choppy audio | expected in Phase 0 (no jitter buffer wired yet); Phase 1 fixes it |

---

## 5. Measure end-to-end latency (the real deliverable)

Phase 0 is one-way audio, so the clean, hardware-free way to get a true
glass-to-glass number is a **dual-recording clap test**:

1. Place a **third device** (a second phone or laptop with a voice recorder) so
   its mic can hear **both** the room directly **and** the PC's speakers. Keep
   all three devices within ~30 cm of each other so air-travel time (~1 ms /
   30 cm) is negligible and roughly cancels.
2. Start the third device recording. **Clap once, sharply.**
3. The recording now has **two transients**: the direct clap, and the same clap
   replayed out the PC speakers after going phone-mic → network → buffer →
   PC-playback.
4. Open the recording in **Audacity**, zoom in, and read the time between the
   two spikes. That gap **is** the end-to-end audio latency.
5. Repeat ~10 times and report the median (and spread — jitter matters as much
   as the mean). Target: **under ~50 ms** on Wi-Fi.

What the number is made of, so you know where to cut if it's high:
`phone capture buffer` + `Oboe/AAudio` + `network (Wi-Fi one-way)` +
`PC ring-buffer fill` + `cpal output buffer`. The two buffer terms dominate and
are the knobs to turn.

> To **decompose** the number later (network vs. buffer vs. device), I can add a
> `--measure` mode to the receiver that logs per-packet arrival jitter and
> ring-buffer residence time, and a round-trip echo so the phone can measure RTT
> directly. Ask for it when you get here.

---

## Definition of done for Phase 0

- You hear your own voice from the PC. ✅ audible loop
- A measured median latency number, with spread, from step 5. 📏

Then Phase 1 begins: wire the jitter buffer (already built and tested in
`pc-app/core/src/jitter_buffer.rs`) into the receive loop, swap PCM16 for Opus
+ PLC, add mDNS discovery, and re-measure.
