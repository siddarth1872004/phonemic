# PhoneMic architecture

## Principles

1. **The hot path is native.** Audio is only touched by C++ (phone engine,
   desktop app). Kotlin does UI, permissions and lifecycle.
2. **Realtime callbacks never block.** The Oboe callback and the sound-card
   callback only touch lock-free rings and atomics: no locks, no allocation,
   no syscalls (except a non-blocking `sem_post` on the phone).
3. **One implementation of the protocol and crypto** (`shared/`), compiled by
   both ends.
4. **Latency is a requirement**, so every buffer is bounded and visible in the UI.
5. **Few, justified dependencies**, fetched at pinned versions.

## Phone (Android)

```
Oboe input (AAudio, 48 kHz, I16, mono, low-latency)
  └─ onAudioReady [realtime]  → peak meter → SPSC ring (32k samples) → sem_post
sender thread
  └─ 10 ms frames → [Opus encode, 48 kbps, FEC] → wire.h AUDIO packet
     (media-clock timestamp, session id)
     → [crypto.h seal] → send() on a UDP socket connect()ed to the PC
  └─ non-blocking recv(): STATUS from the PC → atomics → UI / notification
  └─ reopens the stream after onErrorAfterClose (headset, Bluetooth…)
StreamingService (Kotlin, foreground, type=microphone)
  └─ Wi-Fi low-latency lock, Android NoiseSuppressor/AEC/AGC, notification actions
MainActivity (Kotlin): discovery, PIN, Voice Focus, start/stop/mute, status
```

## Desktop app

```
UDP socket thread ─┐                         ┌─ WebServer (mbedTLS) connection threads
                   ▼                         ▼
              Receiver::process_audio  (one mutex: the pipeline is single-threaded)
                decode (wire.h) → sender lock → auth policy → decrypt (crypto.h)
                → JitterBuffer::pop_ready (reorder ≤2, detect loss)
                → Opus decode (FEC from the next packet on loss)  or
                  PCM16 → float → Resampler (any rate → 48 kHz) → Concealer
                → Denoiser (RNNoise, 480-sample frames)
                → VoiceFx → GainLimiter → mute → peak meter
                → AudioSinkIface::write(mic) [+ monitor]
                                 │
                AudioOutput: AdaptivePlayout (SPSC ring + drift-steered resampler)
                                 │  realtime miniaudio callback
                     virtual cable / PulseAudio sink / BlackHole / speakers
```

| Component | File |
|---|---|
| Protocol, crypto | `shared/include/phonemic/{wire,crypto}.h` |
| Receive pipeline, sender lock, STATUS, discovery replies | `pc-app/core/src/receiver.cpp` |
| Jitter buffer | `pc-app/core/src/jitter_buffer.cpp` |
| Adaptive playout (drift compensation) | `pc-app/core/src/playout.cpp` |
| Resampler, concealment, gain/limiter | `pc-app/core/src/dsp.cpp` |
| Voice effects | `pc-app/core/src/voice_fx.cpp` |
| RNNoise wrapper | `pc-app/core/src/denoise.cpp` |
| Audio devices (miniaudio) | `pc-app/core/src/audio_out.cpp` |
| Virtual microphone per OS, VB-CABLE installer | `pc-app/core/src/virtual_mic.cpp` |
| Browser mic: HTTPS, WebSocket, certificate, QR | `pc-app/core/src/web_server.cpp`, `websocket.cpp` |
| Settings, sockets, utilities | `settings.cpp`, `net.cpp`, `util.cpp` |
| Window | `pc-app/gui/{main,app,ui}.cpp` |
| Driver ring + experimental ACX driver | `pc-app/ipc/`, `pc-app/driver/` |

### Why these buffers

- **Jitter buffer, arrival-driven:** in-order packets play immediately (zero
  added latency); a gap is declared lost only once 2 later packets arrived.
- **Adaptive playout:** the only intentional latency (the "buffer" setting,
  default 40 ms). Its read rate is steered by ≤ 0.4 % to hold the target fill,
  which absorbs both network jitter and phone/PC clock drift. It hard-resyncs if
  the fill exceeds 2 × target + 60 ms.

## Transports

| Transport | Status |
|---|---|
| Wi-Fi UDP (native app) | ✅ |
| WebSocket over TLS (any browser) | ✅ built into the app |
| USB cable (TCP via `adb reverse`, length-prefixed) | ✅ `pc-app/core/src/usb.cpp`; the app keeps `adb reverse` set up |
| Bluetooth RFCOMM | planned |
