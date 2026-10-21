# PhoneMic wire protocol (v2)

The source of truth is [`shared/include/phonemic/wire.h`](../shared/include/phonemic/wire.h).
It is compiled unchanged into the Android engine and the desktop app. If this
document and the header disagree, the header wins.

## Transport

The packets are the same on all three links; only how they are carried differs.

- **Bluetooth:** one RFCOMM connection to the paired PC. The PC publishes an
  SDP record with service UUID **`51f009ff-63ac-465d-a8c5-a9a56f7355ea`**
  (`PM_SERVICE_UUID`), named "PhoneMic"; the phone connects by that UUID, so
  the channel number never matters. Only the OS Bluetooth stacks are used
  (Android, BlueZ, Windows). Pairing authenticates the phone and Bluetooth
  encrypts the link.
- **USB cable:** TCP through `adb reverse`. The phone connects to its own
  `127.0.0.1:4010`; the PC app runs `adb reverse tcp:4010 tcp:<pc port>` and
  listens on `127.0.0.1` only, so the port is not reachable from the network.
- **Wi-Fi:** UDP to port **4010**, one datagram = one packet, no framing. The
  phone finds the PC with HELLO / ANNOUNCE (below), and the payload can be
  sealed with a PIN (below).

RFCOMM and TCP are byte streams, so there every packet is sent as a
**frame**: its length as a little-endian u16, then the packet: `[len][packet]`.
The PC replies with framed STATUS packets on the same connection. A frame
whose length is 0 or larger than 28 + 4096 bytes is not PhoneMic traffic: the
PC closes the connection. It also closes a connection that has sent nothing
for 10 s, and accepts at most 2 connections at a time per link.

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
                        or one Opus packet encoding 10 ms (or ciphertext || tag)
```

**Opus (codec 1):** VOIP mode, 48 kbps, in-band FEC on. The PC always decodes at
48 kHz (`sample_rate` is informational). When a packet is lost the PC rebuilds
it from the *next* packet's FEC data when present (libopus adds it to roughly a
third of voice frames at this bitrate), otherwise from Opus's own concealment.
A session keeps one codec; a codec change starts a new session.

`decode` is total: length ≥ 28, magic, version, kind, codec, no unknown flag bits, session ≠ 0,
rate in range, and `payload_len` must *exactly* equal the bytes present
(truncation and trailing garbage are both rejected). It never reads out of
bounds (fuzzed in `pc-app/tests/test_wire.cpp`).

## Discovery (Wi-Fi)

**HELLO** (phone → `255.255.255.255:4010` and the subnet broadcast), 12 bytes:
`nonce u32`, `reserved u32 = 0`.

**ANNOUNCE** (PC → phone, unicast to the asker), 12 + n bytes:
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
packets are rejected, so the phone can say *why*.

## Encryption (Wi-Fi, optional)

```
key   = Argon2id(passphrase, salt = "phonemic/v2/" || session_id LE,
                 memory 4 MiB, 3 passes, 1 lane) → 32 bytes
nonce = session_id(4) || seq(4) || timestamp_us(8) || 0(8)      (24 bytes)
aad   = the 28-byte AUDIO header
wire  = XChaCha20-Poly1305 ciphertext || 16-byte tag
```

- Per-session keys mean nonces can't repeat across restarts.
- With a PIN set on the PC, **unencrypted Wi-Fi audio is refused**. Bluetooth
  and USB are private links and never need it.
- Argon2id makes each guess expensive, but a short numeric PIN is still
  guessable offline from captured traffic. Use 8+ characters if secrecy matters.
- The PC caches keys per session and derives at most 3 per second.

The implementation is `shared/include/phonemic/crypto.h` (Monocypher), tested in
`pc-app/tests/test_crypto.cpp`.

## Session rules (receiver)

- The first valid sender owns the microphone; other senders get STATUS *busy*
  until the owner has been silent for 2 s.
- A new `session_id` from the same phone (after the 2 s timeout) starts a fresh
  session: the jitter buffer, resampler and playout buffer are reset.
- Packets go through a jitter buffer that tolerates reordering by 2 packets,
  then through loss concealment, then through the adaptive playout buffer that
  compensates clock drift.
