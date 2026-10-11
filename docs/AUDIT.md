# Audit: what was wrong in v0.2, and what v0.3 does about it

This is the list of problems found reviewing the v0.2 code base (Rust desktop
app, Android app, browser client, driver, CI and docs). Every item notes how
v0.3 addresses it. Items are grouped by severity. All of them have regression
tests where it's practical (`pc-app/tests`, now 100 tests).

v0.3 also moves the desktop app from Rust to **C++17** (see the end of this file).

---

## Critical: broken for users

| # | Problem | Fix |
|---|---|---|
| 1 | **Linux: the virtual microphone never got any audio.** The receiver only played audio when `sink.is_virtual_cable` (false on Linux, where the device was chosen through `PULSE_SINK`) or when "Test sound" was ticked, and that checkbox was hidden once the virtual mic existed. "PhoneMic Microphone" was always silent. | Audio is routed by opening the `PhoneMic` sink by name (miniaudio, PulseAudio backend); routing no longer depends on a flag. |
| 2 | **Linux: wrong device name.** pactl registered the source as `PhoneMicMicrophone` (quoting lost the space) while the UI told users to pick "PhoneMic Microphone". The modules were never unloaded, and only the sink's existence was checked (a missing source was never recreated). | Proplist quoted correctly; sink and source checked separately; modules we loaded are unloaded on exit. |
| 3 | **Audio output failed on 44.1 kHz devices.** cpal was forced to 48 kHz with no resampling; WASAPI shared mode rejects that when the mix format is 44.1 kHz (common for VB-CABLE and USB headsets), and the app showed "No audio output" and did nothing. | miniaudio opens the device as 48 kHz mono and converts to whatever the device runs at. |
| 4 | **The desktop app had no jitter buffer.** The tested jitter buffer was only used by the browser gateway; the main app pushed packets straight to the sound card: no reordering, and lost packets became clicks. | Every path goes through the jitter buffer (arrival-driven, tolerates reordering by 2 packets), then packet-loss concealment (replays and fades out instead of cutting to silence). |
| 5 | **No clock-drift compensation.** The phone's and PC's sample clocks differ by 20–200 ppm. A plain 50 ms FIFO therefore either overflows and drops audio or underruns, giving a glitch every few minutes. The gateway's 1 s ring, fed by a 10 ms tokio timer, could instead let latency creep toward 1 s. | New adaptive playout buffer: primes to a target latency, then steers its read rate by ≤0.4 % (inaudible) to hold that latency, and hard-resyncs if far over. Tested with ±300 ppm clocks over 3 simulated minutes. |
| 6 | **Phone: blocking network I/O on the realtime audio thread.** `sendto()` and encryption ran inside Oboe's `onAudioReady()`, contradicting the project's own principle #1. A Wi-Fi stall blocked the capture callback, causing glitches. | The callback only copies samples into a lock-free ring and posts a semaphore. A sender thread does framing, encryption and `send()`. |
| 7 | **Phone: packet size was whatever Oboe delivered** (96 to 2048 frames), not the documented 10 ms. That meant up to 500 packets/s of overhead and odd frame sizes downstream. | The sender thread cuts fixed 10 ms frames (480 samples at 48 kHz). |
| 8 | **Phone: wrong sample rate, so a pitch-shifted voice.** On a mic that can't run at 48 kHz, Oboe opened another rate while the app still labelled the audio 48 kHz. The protocol had no rate field; the browser client sent 44.1 kHz audio as 48 kHz with only a warning. | Oboe resampling is enabled. Protocol v2 carries the sample rate, and the PC resamples any 8–96 kHz input. |
| 9 | **Phone: plugging in a headset or Bluetooth killed capture silently.** There was no error callback, so the stream died while the UI still said "Streaming". | `onErrorAfterClose` reopens the stream on the sender thread. Android voice effects are re-attached to the new session. |
| 10 | **Phone: "Streaming" was shown even when nothing worked.** A failed `NativeBridge.start` (bad IP, mic busy) still showed green "● Streaming to …", and the phone had no way to know whether the PC received anything. | The engine reports start errors. The PC sends STATUS packets (ok / wrong PIN / PIN required / busy / buffer ms / loss). The phone detects "no reply", "no network" and "PC not running PhoneMic" (ICMP refused). |
| 11 | **Phone: rotating the screen lost the state.** After recreation the button read "Start" while the service kept streaming, so tapping it restarted the stream. | UI state is read from the service on every frame. |
| 12 | **Phone: `START_STICKY` restarts could never work.** A null intent hit `return START_NOT_STICKY` without `startForeground`. | `START_NOT_STICKY`, and the service calls `stopSelf()` on a bad intent. |
| 13 | **Android 13+: no notification permission.** The foreground notification was hidden, had no Stop action, and tapping it did nothing. | `POST_NOTIFICATIONS` is requested. The notification has Mute and Stop actions, opens the app, and shows the PC's status. |
| 14 | **Phone: turning Voice Focus "off" didn't turn it off.** The capture always used the `VoiceCommunication` preset, which has the platform's NS/AEC built in. | Off uses the `VoiceRecognition` preset (minimally processed). |

## Security

| # | Problem | Fix |
|---|---|---|
| 15 | **A PIN on the PC didn't stop audio injection.** With a PIN set, unencrypted packets from anyone on the LAN were still played into your microphone. | With a PIN set, unencrypted audio is rejected, and the sender is told why. |
| 16 | **The PIN could be brute-forced instantly.** Key = one BLAKE2b of salt‖PIN, and the PIN field was numeric only, so one captured packet gave up a 6-digit PIN in well under a second. | Argon2id (4 MiB, 3 passes) salted per session; passphrases allowed, and the UI encourages 8+ characters. Derivations on the PC are rate-limited so fake sessions can't pin a CPU. A short PIN is still offline-guessable by a determined attacker, and the docs say so plainly. |
| 17 | **Nonce reuse risk.** The nonce was seq‖timestamp under one static key per PIN; seq restarted at 0 on every Start, and the phone's timestamp is boot-relative. | Per-session keys from a random session id, which is also part of the nonce. |
| 18 | **The crypto interop test asserted nothing.** The "fixed vector" test never pinned a value (comment: "filled in by the interop step"), and CI never ran the C↔Rust cross-checks the README advertised. | Known-answer tests generated with *independent* implementations (reference Argon2 via argon2-cffi, libsodium XChaCha20-Poly1305 via PyNaCl). Phone and PC now compile the same `shared/` crypto code. |
| 19 | **The browser gateway had no authentication.** Anyone on the network could open the page and stream into the PC, and there was no Origin check. | A random pairing code (in the QR / link) is required to open the audio WebSocket, compared in constant time; the Origin is checked; and a strict CSP is set. |
| 20 | **The VB-CABLE installer ran elevated without verification.** It was downloaded and launched elevated with no signature check. | The Authenticode signature must be *Valid* and issued to VB-Audio / Vincent Burel before launch. |
| 21 | **The PIN was cloud-backed-up** (`allowBackup=true`, stored in prefs). | `allowBackup=false` plus data-extraction rules excluding prefs. |
| 22 | **Any device could hijack or garble the stream.** Two senders interleaved their packets into noise. | One sender owns the mic until it has been silent for 2 s; others get "busy". |
| 23 | **CI gave every job `contents: write`**, including pull-request jobs. | Read-only by default; only the tag-release job can write. |
| 24 | The phone accepted any datagram as feedback. | The phone's UDP socket is `connect()`ed to the PC, so only the PC's replies are read. |

## Robustness and UX

| # | Problem | Fix |
|---|---|---|
| 25 | Windows: an ICMP "port unreachable" makes `recvfrom` fail with `WSAECONNRESET`, so the receive loop could spin. | `SIO_UDP_CONNRESET` disabled; a 1 MiB receive buffer. |
| 26 | A second instance silently failed to bind, with no recovery. On Windows, two processes could share the port. | `SO_EXCLUSIVEADDRUSE`; the UI explains and offers port + Retry. |
| 27 | Only one IP was shown (via the 8.8.8.8 route trick). It was wrong with a VPN and missing offline. | All LAN IPv4 addresses are listed (primary first) and refreshed every 5 s, plus **LAN auto-discovery** ("Find my PC"). |
| 28 | Browser client: errors were overwritten. `fail()` called `stop()`, which immediately set "stopped". | Errors stay visible. |
| 29 | Browser client: the AudioContext was created after an `await` (outside the tap), so iOS Safari recorded silence; Firefox threw on a rate mismatch. | Created and resumed inside the tap; Firefox falls back to the device rate (sent in the header). |
| 30 | Browser client: no backpressure over TCP, so latency grew without bound on slow Wi-Fi. There was no wake lock (screen sleep stopped the mic) and no reconnect. | Frames are dropped when `bufferedAmount` > 32 KB; Screen Wake Lock is used; and it reconnects with backoff. |
| 31 | Gateway: all browsers shared one jitter buffer. `--driver` given first was parsed as the bind host. | Per-session sources plus the sender lock; the CLI parser was rewritten. |
| 32 | Gateway: a new self-signed certificate on every start, so a browser warning every time; the SAN was the bind address `0.0.0.0`. | The certificate is generated once (P-256, SAN = localhost + LAN IPs) and kept. |
| 33 | The gateway only played to the speakers, so it was useless as a microphone on Windows without the unfinished driver. | The browser mic is built into the desktop app and uses the same pipeline (virtual mic, Voice Focus, effects). |
| 34 | Settings weren't saved (PIN, Voice Focus, effect…). | `settings.ini` in the per-user config dir; atomic writes, 0600 on POSIX. |
| 35 | The level meter showed the input, not what reaches the mic (after denoise/effects/gain). | Post-processing meter with peak hold, dB scale and a clip colour. |
| 36 | With VB-CABLE installed you couldn't hear yourself (no monitor). | "Listen on this PC" with its own device picker, independent of the mic output. |
| 37 | Peak meter: `-(-32768)` overflowed int16 on the phone. | Computed in int32. |
| 38 | `--measure` compared the phone's monotonic clock to the PC's wall clock, which was meaningless with a real phone, yet documented as working. | Documented as softphone-only (`--wallclock`); the phone now sends a media clock and the PC reports RFC 3550 jitter. |
| 39 | The Rust CLI receiver looked for "cable" on Linux too. | Uses the platform's virtual-mic detection. |
| 40 | The GUI's VB-CABLE "busy" state was detected by comparing a status string. | A proper state flag, shared safely with the installer thread. |

## Driver (experimental, `pc-app/driver`)

| # | Problem | Fix |
|---|---|---|
| 41 | It called ACX functions that don't exist (`AcxDriverConfigInit`, `AcxRtStreamAllocateRtPackets`, single-argument `AcxDeviceInitInitialize`) and created no pins or data formats, so no endpoint could ever appear. | Rewritten against the ACX RT-stream model: circuit, streaming pin, microphone bridge pin, 48 kHz/16-bit/mono format, AllocateRtPackets/FreeRtPackets, GetCapturePacket, presentation position, `AcxRtStreamNotifyPacketComplete`. |
| 42 | **Bugcheck:** the section was opened read-only (`SECTION_MAP_READ`, `PAGE_READONLY`), then the pump wrote `read_index` into it. | Mapped read/write. |
| 43 | Mapped with `ZwMapViewOfSection(ZwCurrentProcess())` into whichever process was current. | `MmMapViewInSystemSpace`. |
| 44 | It read pageable section memory at DISPATCH_LEVEL (a timer DPC). | Pages are locked with an MDL. |
| 45 | The view size was never checked; a smaller section with the same name meant out-of-bounds reads. User-mode indices weren't clamped. | Size and ABI validated; indices clamped; slots taken modulo the capacity. |
| 46 | `Run` failed if the app wasn't running yet; the timer re-armed relative 10 ms (drift). | Silence plus a PASSIVE work item that retries the mapping; absolute schedule from the packet count. |
| 47 | The INF lacked the KS/WDMAUDIO registration that audio drivers need. | Added. |

The driver now compiles and links in CI against the WDK 10.0.26100 headers (ACX 1.0, KMDF 1.33). It has **not been loaded** on real Windows yet and is labelled as such.

### Found while making PhoneMic's own drivers the default

| # | Problem | Fix |
|---|---|---|
| 54 | **Linux: "PhoneMic Microphone" was never created on PulseAudio.** The module argument quoted only the description, PulseAudio rejected `module-remap-source`, and the app still reported the mic as ready. | The whole property list is one quoted argument; a fallback loads the source without properties. CI now records from the mic to prove it. |
| 55 | Linux needed the `pactl` tool, which many distros don't install by default. | libpulse is loaded at run time and driven directly (PulseAudio and PipeWire). |
| 56 | The Windows driver path needed the app to run as administrator (creating `Global\` objects needs a privilege). | The driver creates the ring with a DACL for signed-in users; the app only opens it. |
| 57 | Writing the ring as audio arrived would have under/overrun as the phone's and the driver's clocks drift. | The ring is fed at the driver's pace from the adaptive playout buffer. |
| 58 | The INF needed stampinf placeholders filled and allowed Windows 11 21H2, which lacks KMDF 1.33. | Static `DriverVer`/KMDF version; targets 22H2+ (build 22621). |

## Build, CI and docs

| # | Problem | Fix |
|---|---|---|
| 48 | The release step failed unless someone had created the GitHub release by hand. | `softprops/action-gh-release` creates it. |
| 49 | No Android build in CI; NDK version not pinned. | An Android job (debug + release APK, lint); `ndkVersion` pinned. |
| 50 | `Start-PhoneMic.bat` hard-coded a developer machine's w64devkit path; START-HERE described a "black console window" that no longer existed. | New launchers (`Start-PhoneMic.bat`, `start-phonemic.sh`) and rewritten docs. |
| 51 | The protocol was implemented three times (Rust crate, C header, JS). Docs claimed the Rust crate was compiled into the phone via cbindgen, which never happened. | One C header (`shared/include/phonemic/wire.h`) compiled by the phone and PC; the JS client follows it and is tested against the real server. |
| 52 | Stale, contradictory docs: test counts (13 vs 14), "Phase 0 (current)", "driver not started" next to "code-complete", the ipc README saying "not implemented", PROTOCOL.md missing the encrypted flag, Windows-dev-machine instructions. | Docs rewritten for v0.3. |
| 53 | The default `cargo build`/`test` skipped the GUI and gateway, so CI on PRs didn't build what users run. | One CMake build of everything; CI builds and tests Linux, Windows (MSVC) and macOS, plus ASan/UBSan. |

---

## The desktop app is now C++

| Was (Rust) | Now (C++17) |
|---|---|
| eframe/egui window | Dear ImGui + GLFW + OpenGL 3 |
| cpal output (no resampling) | miniaudio (WASAPI/PulseAudio/ALSA/CoreAudio, converts formats) |
| nnnoiseless (RNNoise port) | RNNoise (the original C library) |
| RustCrypto + separate monocypher on the phone | monocypher, the same `shared/` code on both ends |
| `phonemic-protocol` crate + C mirror + JS | `shared/include/phonemic/wire.h` (phone + PC) |
| tokio/axum/rustls gateway (separate binary) | built-in mbedTLS HTTPS/WebSocket server inside the app |
| ringbuf crate | `SpscRing` (lock-free, ~60 lines) |

Everything is fetched at configure time at pinned versions; the only system
requirements are a C++17 compiler and CMake (plus X11/GL headers on Linux).
