# Tier 2 (native app) & Tier 3 (driver) — hand-off

Both tiers are gated only on tools you install; the code and scaffolding are
done. This is exactly what to do.

---

## Tier 2 — native Android app

The Gradle wrapper is now committed, so the project is turnkey.

### Install
1. **Android Studio** (bundles a JDK 17).
2. SDK Manager → **API 34**, **Build-Tools 34.0.0**, **NDK 26.1.10909125**,
   **CMake 3.22.1**, **Platform-Tools**.
3. Set `ANDROID_HOME`; add `platform-tools` to `PATH` (for `adb`).

### Build & run
```sh
cd phone-app
# create local.properties pointing at your SDK:
#   sdk.dir=C\:\\Users\\Siddarth\\AppData\\Local\\Android\\Sdk
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```
Or just open `phone-app/` in Android Studio and press **Run**. First build is
slow (it compiles the Oboe/native C++ per ABI).

Then follow [PHASE0-BRINGUP.md](PHASE0-BRINGUP.md) §3–5: point the app at the
PC's IP (UDP 4010), run `cargo run -p phonemic-core`, and do the clap test for a
real latency number. The native framing is already proven wire-compatible with
the PC (`native/test/wire_test.cpp` ↔ the Rust decoder).

---

## Tier 3 — the real virtual microphone

This is the biggest lift and the one part with **unverified code** (no WDK here
to compile against). What's done: the full driver source, the shared-ring
contract (Rust side unit-tested), and the gateway `--driver` feed.

### Install
1. **Visual Studio 2022** + "Desktop development with C++".
2. The matching **Windows SDK** and **WDK**, plus the **WDK Visual Studio
   extension** (gives driver templates + ACX headers/libs).
3. Enable test-signing (admin cmd), then reboot:
   ```
   bcdedit /set testsigning on
   ```
   (You may also need Secure Boot off in firmware for a test-signed driver.)

### Build the driver
Open `pc-app/driver/phonemic.vcxproj` in VS. If the project file fights your WDK
version, instead create **New Project → "Kernel Mode Driver, Empty (KMDF)"** and
add `driver.c`, `driver.h`, and `phonemic.inf`.

**Expect to fix compile errors** — these are the spots I couldn't verify without
the WDK:
- `AcxRtStreamAllocateRtPackets` (in `PhonemicEvtStreamPrepare`) — confirm the
  exact ACX packet-allocation API and how packet buffers are retrieved; the
  model (N packets, filled round-robin) is right, the call may need adjusting.
- The RT callback signatures (`EVT_ACX_STREAM_*`) — match them to your WDK's
  `acx.h`.
- `MemoryBarrier()`/volatile access in the pump — fine functionally; tune to the
  WDK's preferred barrier intrinsics.

### Install & run the full chain
```sh
# 1. install the (test-signed) driver
pnputil /add-driver pc-app/driver/phonemic.inf /install

# 2. run the gateway in driver mode, AS ADMIN (it creates the global section)
#    (w64devkit on PATH for the build; see WEB-CLIENT.md)
cargo run -p phonemic-web-gateway -- <PC-LAN-IP> 8443 web-client --driver
```
Then on the phone open `https://<PC-LAN-IP>:8443/`, Start mic. In Discord/Zoom,
pick **"PhoneMic Microphone (virtual)"** as the input.

### Data path (all code present)
```
phone browser → wss → gateway → jitter buffer → phonemic_ipc::Producer
   → Global\PhonemicRing (shared section) → driver maps it → RT capture packets
   → Windows audio stack → "PhoneMic Microphone"
```

The Rust half of that ring (`pc-app/ipc`, 5 tests) is verified; the kernel half
(`driver.c`) is written and needs the WDK to compile, load, and debug.

### Signing for real distribution
Test-signing is dev-only. To ship: attestation-sign via the Windows Hardware Dev
Center (no EV-signed kernel binary needed for attestation), or a full WHQL
submission. See `pc-app/driver/README.md`.
