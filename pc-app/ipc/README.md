# Core ↔ driver IPC (Phase 4 — not implemented yet)

This directory will hold the interface between the Rust core (which receives,
reorders, and decodes audio) and the C ACX virtual audio driver (which exposes
the Windows capture endpoint).

## Decision: shared-memory ring buffer (not private IOCTL)

**Chosen:** a lock-free, single-producer/single-consumer shared-memory ring
carrying continuous PCM frames, mapped between the user-mode core (producer) and
the driver (consumer).

**Why, over a private IOCTL per buffer:**

- Audio is a *continuous* stream, not a request/response workload. A ring lets
  the core write and the driver drain at their own cadences with no per-buffer
  syscall.
- One IOCTL round-trip per audio buffer adds latency and CPU on the hot path —
  directly against principle #2. A mapped ring is a memory write on one side and
  a memory read on the other.
- Back-pressure and underrun are naturally expressed as ring fill level, which
  is also what the jitter buffer already reasons about.

**Cost / caveats to handle when implemented:**

- Lifetime and teardown: the shared section must be safely torn down if either
  side dies. A small control IOCTL is still used to *establish* the mapping and
  exchange the section handle; only the audio data path is the ring.
- Cache-line-aligned head/tail indices with proper memory ordering to stay
  lock-free across the user/kernel boundary.

Layout, exact struct, and the establishing IOCTL will be specified here before
any driver code is written.
