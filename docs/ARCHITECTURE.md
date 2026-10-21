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
6. **Only the operating system's Bluetooth stack.** No Bluetooth driver, no
   Wi-Fi or network code.

## Phone (Android)

```
Oboe input (AAudio, 48 kHz, I16, mono, low-latency)
  └─ onAudioReady [realtime]  → peak meter → SPSC ring (32k samples) → sem_post
sender thread
  └─ 10 ms frames → [Opus encode, 48 kbps, FEC] → wire.h AUDIO packet
     (media-clock timestamp, session id) → [u16 len] frame
     → bounded queue (20 frames, oldest dropped)
  └─ reopens the stream after onErrorAfterClose (headset, Bluetooth…)
StreamingService (Kotlin, foreground, type=microphone)
  └─ BluetoothSocket (RFCOMM, service UUID, paired PC) — Android exposes
     RFCOMM to Java only, so Kotlin owns the socket:
     writer thread: readOutgoing() → socket; reader thread: socket → feedIncoming()
     → STATUS → atomics → UI / notification; reconnects every 2 s on failure
  └─ Android NoiseSuppressor/AEC, notification actions
MainActivity (Kotlin, framework widgets): paired-PC picker, Voice Focus,
  start/stop/mute, status
```

## Desktop app

```
BluetoothServer: publishes the "PhoneMic" RFCOMM service through the OS stack
  (BlueZ Profile1 over D-Bus · Winsock AF_BTH + WSASetService · IOBluetooth)
  └─ one thread per connection: serve_stream() → FrameSplitter → packets
                   ▼                         ▲ framed STATUS, 2 Hz
              Receiver::process_audio  (one mutex: the pipeline is single-threaded)
                decode (wire.h) → sender lock (one phone at a time)
                → JitterBuffer::pop_ready (reorder ≤2, detect loss)
                → Opus decode (FEC from the next packet on loss)  or
                  PCM16 → float → Resampler (any rate → 48 kHz) → Concealer
                → Denoiser (RNNoise, 480-sample frames)
                → VoiceFx → GainLimiter → mute → peak meter
                → AudioSinkIface::write(mic) [+ monitor]
                                 │
                AudioOutput: AdaptivePlayout (SPSC ring + drift-steered resampler)
                                 │  realtime miniaudio callback, or on Windows
                                 │  RingFeeder → shared ring → ACX driver
                     PhoneMic Microphone (own driver / PipeWire / PulseAudio)
                     · or a chosen speaker
```

| Component | File |
|---|---|
| Protocol | `shared/include/phonemic/wire.h` |
| Bluetooth service, framing, per-connection pump | `pc-app/core/src/bluetooth.cpp` |
| Bluetooth per OS | `bluetooth_linux.cpp` (BlueZ, libdbus loaded at run time), `bluetooth_win.cpp`, `bluetooth_mac.mm` |
| Receive pipeline, sender lock, STATUS | `pc-app/core/src/receiver.cpp` |
| Jitter buffer | `pc-app/core/src/jitter_buffer.cpp` |
| Adaptive playout (drift compensation) | `pc-app/core/src/playout.cpp` |
| Resampler, concealment, gain/limiter | `pc-app/core/src/dsp.cpp` |
| Voice effects | `pc-app/core/src/voice_fx.cpp` |
| RNNoise wrapper | `pc-app/core/src/denoise.cpp` |
| Audio devices (miniaudio) | `pc-app/core/src/audio_out.cpp` |
| Virtual microphone per OS, driver installers | `pc-app/core/src/virtual_mic.cpp` |
| Linux sound-server control (libpulse, loaded at run time) | `pc-app/core/src/pulse_ctl.cpp` |
| Settings, helper processes, utilities | `settings.cpp`, `process.cpp`, `util.cpp` |
| Window | `pc-app/gui/{main,app,ui}.cpp` |
| Windows driver (ACX) + its ring and feeder | `pc-app/driver/`, `pc-app/ipc/` |
| macOS driver (Core Audio plug-in) | `pc-app/driver-macos/` |
| Linux packaging | `packaging/linux/` |

### Virtual microphone

| OS | How | Apps record |
|---|---|---|
| Windows 11 | PhoneMic ACX driver; the app writes into the ring the driver creates | PhoneMic Microphone |
| macOS | PhoneMic.driver loopback device; the app plays into its output side | PhoneMic Microphone |
| Linux | PipeWire/PulseAudio null sink + remapped source, created through libpulse | PhoneMic Microphone |
| Linux, ALSA only | the kernel's `snd-aloop` loopback card | Loopback device 1 |

No third-party virtual-cable driver is used or supported. `default_mic.cpp` makes
PhoneMic the system default recording device (Windows: all three roles;
macOS: default input; Linux: default source), for apps without a mic picker.
On Linux the mic can stay registered when PhoneMic is closed (a pipewire-pulse
drop-in, or a marked `.nofail` block in `~/.config/pulse/default.pa`).

### Why these buffers

- **Jitter buffer, arrival-driven:** in-order packets play immediately (zero
  added latency); a gap is declared lost only once 2 later packets arrived.
- **Adaptive playout:** the only intentional latency (the "buffer" setting,
  default 40 ms). Its read rate is steered by ≤ 0.4 % to hold the target fill,
  which absorbs both Bluetooth jitter and phone/PC clock drift. It hard-resyncs if
  the fill exceeds 2 × target + 60 ms.

## Transport

Bluetooth RFCOMM only, through each operating system's own stack, so no driver
is installed for it. The phone connects to the paired PC by service UUID
(`PM_SERVICE_UUID`); the PC publishes the service record. Pairing
authenticates the phone and the link is encrypted by Bluetooth, so PhoneMic
adds no crypto of its own.

The phone does not present itself as a Bluetooth headset (HFP/HSP): Android
apps cannot take that role, and headset audio is 16 kHz at best. The audio
reaches apps through the PhoneMic Microphone above instead.

At about 48 kbps, Opus fits comfortably in a classic Bluetooth link. The
phone keeps at most 20 frames (200 ms) queued and drops the oldest when the
link stalls, so latency stays bounded instead of growing.
