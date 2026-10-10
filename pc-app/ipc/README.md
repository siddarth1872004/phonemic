# Core ↔ driver IPC (shared-memory ring)

The interface between the desktop app (which receives, reorders and cleans up
the audio) and the experimental ACX virtual-microphone driver in `../driver`.

- `ring.h` — the canonical layout, compiled by **both** the kernel driver (C) and
  the app (C++). One file, so the two sides cannot drift.
- `ring_producer.hpp` — the user-mode writer. `static_assert`s the layout offsets
  and is unit-tested (`pc-app/tests/test_ipc.cpp`) against a consumer that mirrors
  the driver's read loop.
- `driver_feed_win.cpp` — maps `Global\PhonemicRing` and feeds it; used by
  `phonemic-receiver --driver` (Windows, administrator).

## Why a shared-memory ring (not an IOCTL per buffer)

Audio is a continuous stream, not request/response. A lock-free single-producer /
single-consumer ring lets the app write and the driver drain at their own cadence
with no syscall per buffer and no locks on the audio path. Indices are
monotonically increasing sample counters (`slot = index % capacity`); the producer
stores samples then publishes `write_index` (release), the consumer acquires
`write_index` before reading and publishes `read_index` back.

The driver never trusts the indices for memory safety: every slot access is taken
modulo the capacity, and the mapped view's size is checked before use.
