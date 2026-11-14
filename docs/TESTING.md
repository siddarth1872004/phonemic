# Testing

## Automated

```sh
ctest --test-dir build --output-on-failure
./build/bin/phonemic-tests jb_        # run a subset by name
```

| Area | What's covered |
|---|---|
| `test_wire.cpp` | packet layout, every rejection path, the frame prefix, STATUS messages, 20,000-case fuzz (no out-of-bounds) |
| `test_bluetooth.cpp` | frame splitting across arbitrary chunks, the per-connection pump (framed phone audio → microphone, STATUS answered on the same stream), non-PhoneMic streams dropped, the socket stream closing from another thread, the process runner |
| `test_jitter.cpp` | all v0.2 jitter-buffer cases + arrival-driven mode |
| `test_playout.cpp` | ±300 ppm clock drift for 3 simulated minutes, priming/underrun, burst resync, flush |
| `test_dsp.cpp` | resampler pitch/length/continuity, loss concealment fades, limiter never clips |
| `test_voice_fx.cpp`, `test_denoise.cpp` | pitch ratios by zero crossings, filter response, echo impulse response, RNNoise noise reduction |
| `test_receiver.cpp` | end-to-end pipeline: reorder + loss, sender lock, session takeover, resampling, mute/gain, garbage counted, STATUS per session |
| `test_opus.cpp` | Opus round trip (pitch, < 70 kbps), receiver decoding, a dropped packet rebuilt from FEC, corrupt packet concealed |
| `test_settings.cpp` | settings round trip |
| `test_persistent_mic.cpp` | Linux "keep the mic when PhoneMic is closed": PipeWire drop-in, PulseAudio block appended and removed byte-for-byte, refusal without a system config |
| `test_usb.cpp` | framed audio over TCP plays and is answered with STATUS; the listener accepts loopback only |
| `test_wifi.cpp` | HELLO answered with ANNOUNCE, UDP audio, the PIN policy (plaintext refused when a PIN is set, wrong PIN reported); a second server can't take the same port |
| `test_crypto.cpp` | Argon2id and XChaCha20-Poly1305 against reference / libsodium vectors, round trip, every auth failure, per-session keys, nonce layout |
| `test_wire_c99.c` | the shared headers compile as strict C99 |

CI also runs the suite under **AddressSanitizer + UndefinedBehaviorSanitizer**,
on Windows (MSVC), and builds the Android app with lint.

The phone's native engine (Opus framing, the bounded send queue, STATUS
parsing in any chunking) is plain C++ apart from Oboe, so it can also be
exercised on a desktop with a stub `oboe/Oboe.h` and ASan.

### The virtual microphone, for real

`phonemic-vmic-check --setup` runs the app's own virtual-mic setup, plays a
997 Hz tone into it, records the microphone apps would pick, and passes only if
the tone comes back (level and spectrum). CI runs it:

- on Ubuntu with PulseAudio;
- after installing the `.deb`/`.rpm`/tarball on Ubuntu 22.04 (PulseAudio),
  Debian 12, Fedora 41, Arch Linux and openSUSE Tumbleweed (PipeWire), where the
  test suite also runs; and on Rocky Linux 8 (glibc 2.28, the oldest supported)
  the package install and the test suite.

Every run is done twice: recording "PhoneMic Microphone" by name, and with
`--default`, which makes PhoneMic the system's default microphone and records
the *default* device, like an app without a microphone picker. The Linux job also
turns on "keep it when PhoneMic is closed", restarts PulseAudio and checks the
microphone is there without PhoneMic running.

Run it yourself to check a setup: `phonemic-vmic-check --setup [--default]`
(or `--list`). On Windows it plays into VB-CABLE and records its other side
(either name, before or after **Rename to PhoneMic**). The VB-CABLE install,
rename and default-speaker code has no automated tests.

## With a phone

1. Run the receiver without touching the system microphone:
   ```sh
   ./build/bin/phonemic-receiver --null           # Bluetooth + Wi-Fi
   ./build/bin/phonemic-receiver --null --usb     # also USB (needs adb)
   ./build/bin/phonemic-receiver --null --pin 1234
   ```
   While the phone streams it prints the packets, loss, jitter and buffer
   every 5 s.
2. In the app pick the link and tap **Start**:
   - **Bluetooth:** pair the phone with the PC first, then choose the PC.
   - **Wi-Fi:** **Find your PC**, or enter the address. With `--pin`, try the
     right PIN, a wrong one and none: the phone should explain each.
   - **USB:** plug in a phone with USB debugging allowed.
3. Turning the link off and on (Bluetooth, Wi-Fi, unplugging the cable) or
   closing and reopening the receiver checks that the phone reconnects by
   itself.

On Linux, `bluetoothctl show` confirms the adapter is powered, and
`journalctl -u bluetooth` shows BlueZ's view of the connection.

### Soundboard

Needs a real phone; the WebView part cannot be checked on the PC.

1. Start streaming (any link) and on the PC open a recorder on
   "PhoneMic Microphone" (or enable **Listen on this PC**).
2. Open **Soundboard** and wait for the page to stop being dimmed. Tap
   **VINE BOOM**: the PC hears it, and the phone's speaker stays silent.
3. Talk while tapping sounds: voice and sound are both heard, without clipping
   or wrapping noise, even with **Level** at 200%.
4. Tap a long sound, then another: the first stops at once. **Stop** silences
   the current one.
5. Stop streaming and tap a sound: a "Not streaming" message appears and nothing
   plays anywhere. Start again: no leftover of the earlier sound.
6. Mute: sounds are muted too, and do not play late after unmuting.
7. Rotate the phone during a sound: the page is not reloaded and the sound
   continues. Press Home: streaming and the current sound continue.
8. Turn data off and reopen the screen: an error with **Retry** is shown.
9. Follow a link off MyInstants (an ad): it does not open.
10. Renderer crash (emulator or rooted phone, after `adb root`): find the
    app's `sandboxed_process` with `adb shell ps -A` and `adb shell kill` it.
    The app keeps running, logcat shows "WebView renderer gone" and the screen
    offers **Retry**.

### Real end-to-end latency (clap test)

1. Put a third device that records audio (another phone/laptop) next to the
   phone and the PC speakers, and enable **Listen on this PC**.
2. Start recording and clap once, sharply.
3. The recording has two spikes: the clap itself and the same clap from the
   PC speakers. Open it in Audacity; the gap between the spikes is the
   end-to-end latency.
4. Repeat ~10 times; report the median and spread.
