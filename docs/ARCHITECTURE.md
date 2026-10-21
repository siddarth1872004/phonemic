# PhoneMic architecture

## Principles

1. **The hot path is native.** Audio is only touched by C++ (phone engine,
   desktop app). Kotlin does UI, permissions and lifecycle.
2. **Realtime callbacks never block.** The Oboe callback and the sound-card
   callback only touch lock-free rings and atomics: no locks, no allocation,
   no syscalls (except a non-blocking `sem_post` on the phone).
3. **One implementation of the protocol** (`shared/`), compiled by both ends.
4. **Latency is a requirement**, so every buffer is bounded and visible in the UI.
5. **Few, justified dependencies**, fetched at pinned versions. The Android app
   uses framework widgets only (no AndroidX or Material), and the desktop app
   loads optional system libraries (libpulse, libdbus, Wayland) at run time.
6. **No drivers of our own.** Bluetooth uses the operating system's stack;
   on Windows the microphone is VB-CABLE, installed from the app.

## Phone (Android)

```
Oboe input (AAudio, 48 kHz, I16, mono, low-latency)
  └─ onAudioReady [realtime]  → peak meter → SPSC ring (32k samples) → sem_post
sender thread
  └─ 10 ms frames → [Opus encode, 48 kbps, FEC] → wire.h AUDIO packet
     (media-clock timestamp, session id) → [u16 len] frame
     → bounded queue (20 frames, oldest dropped)
  └─ reopens the stream after onErrorAfterClose (headset, Bluetooth…)
     (optionally sealed with the PIN on Wi-Fi, crypto.h)
StreamingService (Kotlin, foreground, type=microphone)
  └─ one Conn per link, owned by Kotlin (Android exposes RFCOMM to Java only):
       BluetoothConn  RFCOMM to the paired PC, by service UUID
       UsbConn        TCP to 127.0.0.1:4010 (adb reverse), tcpNoDelay
       WifiConn       UDP to the PC, one datagram per packet (strips/adds the
                      u16 prefix), holding a low-latency Wi-Fi lock
     writer thread: readOutgoing() → conn; reader thread: conn → feedIncoming()
     → STATUS → atomics → UI / notification; reconnects every 2 s on failure
  └─ Android NoiseSuppressor/AEC, notification actions
MainActivity (Kotlin, framework widgets): link picker (Bluetooth / Wi-Fi /
  USB), paired-PC picker, Find your PC (Discovery.kt, HELLO/ANNOUNCE), PIN,
  Voice Focus, start/stop/mute, status, Appearance (theme + accent)
```

## Desktop app

```
BluetoothServer: publishes the "PhoneMic" RFCOMM service through the OS stack
  (BlueZ Profile1 over D-Bus · Winsock AF_BTH + WSASetService)
UsbServer: TCP on 127.0.0.1 only; AdbBridge keeps `adb reverse` mapped
  └─ one thread per connection: serve_stream() → FrameSplitter → packets
WifiServer: UDP :4010, answers HELLO with ANNOUNCE (≤ 10/s), one packet per datagram
                   ▼                         ▲ STATUS, 2 Hz
              Receiver::process_audio  (one mutex: the pipeline is single-threaded)
                decode (wire.h) → open sealed payload (PIN, Wi-Fi)
                → sender lock (one phone at a time)
                → JitterBuffer::pop_ready (reorder ≤2, detect loss)
                → Opus decode (FEC from the next packet on loss)  or
                  PCM16 → float → Resampler (any rate → 48 kHz) → Concealer
                → Denoiser (RNNoise, 480-sample frames)
                → VoiceFx → GainLimiter → mute → peak meter
                → AudioSinkIface::write(mic) [+ monitor]
                                 │
                AudioOutput: AdaptivePlayout (SPSC ring + drift-steered resampler)
                                 │  realtime miniaudio callback
                     VB-CABLE (Windows) / PipeWire / PulseAudio
                     → PhoneMic Microphone · or a chosen speaker
```

| Component | File |
|---|---|
| Protocol | `shared/include/phonemic/wire.h` |
| Bluetooth service, framing, per-connection pump | `pc-app/core/src/bluetooth.cpp` |
| Bluetooth per OS | `bluetooth_linux.cpp` (BlueZ, libdbus loaded at run time), `bluetooth_win.cpp` |
| USB (TCP server, `adb reverse`) | `pc-app/core/src/usb.cpp` |
| Wi-Fi (UDP, discovery) and sockets | `pc-app/core/src/wifi.cpp`, `net.cpp` |
| PIN encryption | `shared/include/phonemic/crypto.h` (Monocypher) |
| Receive pipeline, sender lock, STATUS | `pc-app/core/src/receiver.cpp` |
| Jitter buffer | `pc-app/core/src/jitter_buffer.cpp` |
| Adaptive playout (drift compensation) | `pc-app/core/src/playout.cpp` |
| Resampler, concealment, gain/limiter | `pc-app/core/src/dsp.cpp` |
| Voice effects | `pc-app/core/src/voice_fx.cpp` |
| RNNoise wrapper | `pc-app/core/src/denoise.cpp` |
| Audio devices (miniaudio) | `pc-app/core/src/audio_out.cpp` |
| Virtual microphone per OS, VB-CABLE install / rename / default-speaker fix | `pc-app/core/src/virtual_mic.cpp` |
| Linux sound-server control (libpulse, loaded at run time) | `pc-app/core/src/pulse_ctl.cpp` |
| Settings, helper processes, utilities | `settings.cpp`, `process.cpp`, `util.cpp` |
| Window | `pc-app/gui/{main,app,ui}.cpp` |
| Linux packaging | `packaging/linux/` |

### Virtual microphone

| OS | How | Apps record |
|---|---|---|
| Windows 10 / 11 | VB-CABLE: the app plays into "CABLE Input" | CABLE Output, or PhoneMic Microphone once renamed |
| Linux | PipeWire/PulseAudio null sink + remapped source, created through libpulse | PhoneMic Microphone |
| Linux, ALSA only | the kernel's `snd-aloop` loopback card | Loopback device 1 |

PhoneMic ships no driver. On Windows the app installs VB-CABLE on request:
it downloads the installer from vb-audio.com, checks its Authenticode signature
(valid, issued to VB-Audio) and runs it elevated. **Rename to PhoneMic** sets
the two endpoints' `PKEY_Device_DeviceDesc` to "PhoneMic Feed" and "PhoneMic
Microphone" (an elevated copy of the app, `--rename-cable` / `--restore-cable`);
the driver's own name stays, since its signed INF can't change. If Windows
makes the cable the default speaker, the app moves the default back to a real
device so PC sound doesn't leak into the mic. `default_mic.cpp` makes
PhoneMic the system default recording device (Windows: all three roles;
Linux: default source), for apps without a mic picker.
On Linux the mic can stay registered when PhoneMic is closed (a pipewire-pulse
drop-in, or a marked `.nofail` block in `~/.config/pulse/default.pa`).

### Why these buffers

- **Jitter buffer, arrival-driven:** in-order packets play immediately (zero
  added latency); a gap is declared lost only once 2 later packets arrived.
- **Adaptive playout:** the only intentional latency (the "buffer" setting,
  default 40 ms). Its read rate is steered by ≤ 0.4 % to hold the target fill,
  which absorbs both link jitter and phone/PC clock drift. It hard-resyncs if
  the fill exceeds 2 × target + 60 ms.

## Transport

| Link | Carried as | Security |
|---|---|---|
| Bluetooth | RFCOMM by service UUID (`PM_SERVICE_UUID`), framed | pairing + Bluetooth link encryption |
| USB | TCP via `adb reverse` to 127.0.0.1, framed | never leaves the cable; PC listens on loopback only |
| Wi-Fi | UDP :4010, one packet per datagram | optional PIN: XChaCha20-Poly1305, Argon2id key |

The phone does not present itself as a Bluetooth headset (HFP/HSP): Android
apps cannot take that role, and headset audio is 16 kHz at best. The audio
reaches apps through the PhoneMic Microphone above instead.

At about 48 kbps, Opus fits comfortably in a classic Bluetooth link. The
phone keeps at most 20 frames (200 ms) queued and drops the oldest when the
link stalls, so latency stays bounded instead of growing.
