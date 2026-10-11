# Core ↔ driver IPC (shared-memory ring)

The interface between the desktop app (which receives, reorders and cleans up
the audio) and the experimental ACX virtual-microphone driver in `../driver`.

- `ring.h` — the canonical layout, compiled by **both** the kernel driver (C) and
  the app (C++). One file, so the two sides cannot drift.
- `ring_producer.hpp` — the user-mode writer. `static_assert`s the layout offsets
  and is unit-tested (`pc-app/tests/test_ipc.cpp`) against a consumer that mirrors
  the driver's read loop.
- `ring_feeder.*` — keeps the ring topped up (~20 ms) from an `AdaptivePlayout`,
  so the driver's clock paces the stream and drift is absorbed (tested with a
  simulated driver 300 ppm off for three minutes).
- `driver_feed_win.cpp` — opens `Global\PhonemicRing` (created by the driver;
  no administrator rights needed) and runs a `RingFeeder` on it. The desktop app
  and `phonemic-receiver` use it whenever the driver is installed.

## Why a shared-memory ring (not an IOCTL per buffer)

Audio is a continuous stream, not request/response. A lock-free single-producer /
single-consumer ring lets the app write and the driver drain at their own cadence
with no syscall per buffer and no locks on the audio path. Indices are
monotonically increasing sample counters (`slot = index % capacity`); the producer
stores samples then publishes `write_index` (release), the consumer acquires
`write_index` before reading and publishes `read_index` back.

The driver never trusts the indices for memory safety: every slot access is taken
modulo the capacity, and the mapped view's size is checked before use.
