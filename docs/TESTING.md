# Testing

## Automated

```sh
ctest --test-dir build --output-on-failure
./build/bin/phonemic-tests jb_        # run a subset by name
```

| Area | What's covered |
|---|---|
| `test_wire.cpp` | v2 + legacy v1 framing, every rejection path, discovery/status messages, 20,000-case fuzz (no out-of-bounds) |
| `test_crypto.cpp` | Argon2id and XChaCha20-Poly1305 **known-answer vectors from independent implementations**, wrong PIN, tampered header, replay under another seq |
| `test_jitter.cpp` | all v0.2 jitter-buffer cases + arrival-driven mode |
| `test_playout.cpp` | ±300 ppm clock drift for 3 simulated minutes, priming/underrun, burst resync, flush |
| `test_dsp.cpp` | resampler pitch/length/continuity, loss concealment fades, limiter never clips |
| `test_voice_fx.cpp`, `test_denoise.cpp` | pitch ratios by zero crossings, filter response, echo impulse response, RNNoise noise reduction |
| `test_receiver.cpp` | end-to-end pipeline: reorder + loss, PIN policy, sender lock, resampling, legacy phones, and over real UDP: discovery and STATUS feedback |
| `test_web_server.cpp` | real TLS: page + CSP, wrong pairing code → 403, WebSocket upgrade, audio delivered |
| `test_websocket.cpp`, `test_settings.cpp`, `test_ipc.cpp` | SHA-1/Base64/RFC 6455 vectors, frame parser, settings round trip, driver ring |
| `test_wire_c99.c` | the shared headers compile as strict C99 |

CI also runs the suite under **AddressSanitizer + UndefinedBehaviorSanitizer**,
on Windows (MSVC) and macOS, and builds the Android app with lint.

## Without a phone

```sh
# terminal 1: the receiver, discarding audio
./build/bin/phonemic-receiver --null --pin "test pass"
# terminal 2: a fake phone that finds the PC, encrypts, and simulates bad Wi-Fi
./build/bin/phonemic-softphone --discover --pin "test pass" --loss 2 --reorder 3
```

The softphone prints the PC's STATUS feedback ("ok", "PIN mismatch", "busy"…).

### Software latency

```sh
./build/bin/phonemic-receiver --measure 300 &
./build/bin/phonemic-softphone 127.0.0.1 --wallclock
```

This measures transport + processing only (loopback median ≈ 0.1 ms). Add the
playout buffer setting and, with a real phone, capture and Wi-Fi time.

### Real end-to-end latency (clap test)

1. Put a third device that records audio (another phone/laptop) next to the
   phone and the PC speakers, and enable **Listen on this PC**.
2. Start recording and clap once, sharply.
3. The recording has two spikes: the clap itself and the same clap from the
   PC speakers. Open it in Audacity; the gap between the spikes is the
   end-to-end latency.
4. Repeat ~10 times; report the median and spread.

## Browser client

The browser page can be checked in headless Chromium with a fake microphone
(`--use-fake-device-for-media-stream --use-fake-ui-for-media-stream`,
`ignoreHTTPSErrors`) against `phonemic-receiver --null --web`.
