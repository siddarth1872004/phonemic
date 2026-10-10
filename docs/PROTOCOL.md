# PhoneMic wire protocol (v2)

The source of truth is [`shared/include/phonemic/wire.h`](../shared/include/phonemic/wire.h)
(framing) and [`crypto.h`](../shared/include/phonemic/crypto.h) (encryption). Both
are compiled unchanged into the Android engine and the desktop app; the browser
client (`web-client/app.js`) follows the same layout and is tested against the
real server. If this document and the headers disagree, the headers win.

## Transport

- Native app → PC: **UDP**, default port **4010**. One datagram = one packet.
- Browser → PC: **WebSocket over TLS** (`wss://<pc>:8443/ws?t=<pairing code>`),
  one binary message = one AUDIO packet (no encryption flag: TLS covers it).

All multi-byte integers are **little-endian** (both target CPUs are LE; the
encoders are byte-wise, so they are correct anywhere). `magic` 0x4D50 puts the
ASCII bytes `"PM"` first on the wire.

## Common prefix (4 bytes)

```
off size field
 0   2   magic    0x4D50 ("PM")
 2   1   version  2   (1 = legacy, see below)
 3   1   kind     0 AUDIO, 1 HELLO, 2 ANNOUNCE, 3 STATUS
```

## AUDIO (phone → PC), 28-byte header

```
 4   1   codec         0 = PCM16, 1 = Opus
 5   1   flags         bit0 = encrypted; other bits must be 0
 6   2   payload_len   bytes after the header (incl. 16-byte tag if encrypted)
 8   4   session_id    random per stream start, never 0
12   4   seq           per session, from 0
16   8   timestamp_us  sender media clock: capture start + samples_sent / rate
24   4   sample_rate   8000..96000 Hz (the PC resamples to 48 kHz)
28  ...  payload       mono PCM16 LE, normally 10 ms (480 samples at 48 kHz),
                        or one Opus packet encoding 10 ms
```

**Opus (codec 1):** VOIP mode, 48 kbps, in-band FEC on. The PC always decodes at
48 kHz (`sample_rate` is informational). When a packet is lost the PC rebuilds
it from the *next* packet's FEC data when present (libopus adds it to roughly a
third of voice frames at this bitrate), otherwise from Opus's own concealment.
A session keeps one codec; a codec change starts a new session.

`decode` is total: length ≥ 28, magic, version, kind, codec, flags, session ≠ 0,
rate in range, and `payload_len` must *exactly* equal the bytes present
(truncation and trailing garbage are both rejected). It never reads out of
bounds (fuzzed in `pc-app/tests/test_wire.cpp`).

## Discovery

**HELLO** (phone → `255.255.255.255:4010` and the subnet broadcast), 12 bytes:
`nonce u32`, `reserved u32 = 0`.

**ANNOUNCE** (PC → phone, unicast), 12 + n bytes:
`nonce u32` (copied from HELLO), `audio_port u16`, `flags u8`
(bit0 PIN required, bit1 busy), `name_len u8` (≤ 63), `name` (UTF-8 host name).
The PC answers at most 10 HELLOs per second.

## STATUS (PC → phone, about 2 Hz), 24 bytes

```
 4   1   state       0 ok, 1 PIN required, 2 PIN missing on PC, 3 PIN wrong,
                     4 busy (another device streaming), 5 unsupported format
 5   1   reserved
 6   2   buffer_ms   PC playout latency
 8   4   session_id  the session the report is about
12   4   received    AUDIO packets accepted this session
16   4   lost        packets concealed this session
20   4   reserved
```

Sent every 500 ms to the active sender, and (rate-limited) to any sender whose
packets are rejected, so the phone can say *why*. The phone's socket is
`connect()`ed to the PC, so it only ever reads the PC's replies.

## Encryption (optional)

```
key   = Argon2id(passphrase, salt = "phonemic/v2/" || session_id LE,
                 memory 4 MiB, 3 passes, 1 lane) → 32 bytes
nonce = session_id(4) || seq(4) || timestamp_us(8) || 0(8)      (24 bytes)
aad   = the 28-byte AUDIO header
wire  = XChaCha20-Poly1305 ciphertext || 16-byte tag
```

- Per-session keys mean nonces can't repeat across restarts.
- With a passphrase set on the PC, **unencrypted UDP audio is refused**.
- Argon2id makes each guess expensive, but a short numeric PIN is still
  guessable offline from captured traffic. Use 8+ characters if secrecy matters.
- The PC caches keys per session and derives at most 3 per second.

Known-answer vectors from independent implementations (reference Argon2,
libsodium) are pinned in `pc-app/tests/test_crypto.cpp`.

## Session rules (receiver)

- The first valid sender owns the microphone; other senders get STATUS *busy*
  until the owner has been silent for 2 s.
- A new `session_id` from the same phone (after the 2 s timeout) starts a fresh
  session: the jitter buffer, resampler and playout buffer are reset.
- Packets go through a jitter buffer that tolerates reordering by 2 packets,
  then through loss concealment, then through the adaptive playout buffer that
  compensates clock drift.

## Legacy v1

v0.2 phones sent an 18-byte header: `magic, version=1, codec|0x80 (encrypted),
seq u32, timestamp u64, payload_len u16`. The PC still accepts **unencrypted** v1
audio (assumed 48 kHz) so an old phone keeps working. Encrypted v1 is refused
(its key derivation was weak) and the UI asks you to update the phone app.
