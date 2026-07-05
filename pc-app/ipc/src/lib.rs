//! User-mode side of the core ↔ driver shared-memory ring.
//!
//! This is the Rust mirror of `ring.h` — the **exact same layout** — plus a
//! [`Producer`] that writes PCM into a ring the driver consumes. The struct is
//! `#[repr(C)]` with explicit padding so a pointer to the mapped section can be
//! cast straight to [`Ring`].
//!
//! Discipline: single-producer (this side) / single-consumer (the driver). The
//! producer publishes samples, then releases `write_index`; the consumer
//! acquires `write_index` before reading. See `ipc/README.md`.

use std::sync::atomic::{fence, Ordering};

/// Ring capacity in samples (1 s of mono 48 kHz). Mirrors `PHONEMIC_RING_CAPACITY`.
pub const CAPACITY: usize = 48_000;
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u32 = 1;
/// Layout/ABI version; both sides reject a mismatch during the mapping handshake.
pub const ABI_VERSION: u32 = 1;

/// Exact mirror of `PHONEMIC_RING` in `ring.h`. Do not reorder fields.
#[repr(C)]
pub struct Ring {
    pub abi_version: u32,
    pub capacity: u32,
    pub sample_rate: u32,
    pub channels: u32,
    _pad0: [u8; 64 - 16],
    /// Producer-owned: total samples ever written. Accessed volatilely.
    pub write_index: u64,
    _pad1: [u8; 64 - 8],
    /// Consumer-owned: total samples ever read. Accessed volatilely.
    pub read_index: u64,
    _pad2: [u8; 64 - 8],
    pub samples: [i16; CAPACITY],
}

impl Ring {
    /// Initialise a freshly-mapped (zeroed) ring's header. Called once by
    /// whichever side owns creation before the other attaches.
    pub fn init_header(&mut self) {
        self.abi_version = ABI_VERSION;
        self.capacity = CAPACITY as u32;
        self.sample_rate = SAMPLE_RATE;
        self.channels = CHANNELS;
        self.write_index = 0;
        self.read_index = 0;
    }

    /// True if the header matches what this build expects.
    pub fn header_ok(&self) -> bool {
        self.abi_version == ABI_VERSION
            && self.capacity == CAPACITY as u32
            && self.sample_rate == SAMPLE_RATE
            && self.channels == CHANNELS
    }
}

/// Writes PCM samples into a shared ring for the driver to consume.
///
/// Holds a raw pointer to a [`Ring`] living in a shared section. Constructing
/// one is `unsafe`: the caller guarantees the pointer is valid, aligned, and the
/// sole producer for the ring's lifetime.
pub struct Producer {
    ring: *mut Ring,
}

// The producer is the only writer; it is safe to move between threads as long as
// it remains the single producer (the caller's responsibility, as with any SPSC).
unsafe impl Send for Producer {}

impl Producer {
    /// # Safety
    /// `ring` must point to a valid, aligned [`Ring`] in memory shared with the
    /// consumer, and this must be the only `Producer` for it.
    pub unsafe fn new(ring: *mut Ring) -> Self {
        Producer { ring }
    }

    #[inline]
    fn ring(&self) -> &Ring {
        // SAFETY: validity guaranteed by the `new` contract.
        unsafe { &*self.ring }
    }

    /// Samples the consumer hasn't read yet (i.e. currently occupying the ring).
    pub fn occupied(&self) -> u64 {
        // Acquire the consumer's progress before trusting free space.
        let read = unsafe { std::ptr::read_volatile(&self.ring().read_index) };
        fence(Ordering::Acquire);
        let write = self.ring().write_index;
        write.wrapping_sub(read)
    }

    /// Free space in samples.
    pub fn vacant(&self) -> u64 {
        CAPACITY as u64 - self.occupied()
    }

    /// Push as many of `samples` as fit; returns the count accepted. Fewer than
    /// `samples.len()` means the consumer is behind and the rest was dropped
    /// (real-time: never block the producer).
    pub fn push(&mut self, samples: &[i16]) -> usize {
        let vacant = self.vacant() as usize;
        let n = samples.len().min(vacant);
        if n == 0 {
            return 0;
        }

        // SAFETY: single producer; we only write slots the consumer has freed.
        let ring = unsafe { &mut *self.ring };
        let write = ring.write_index;
        for (i, &s) in samples[..n].iter().enumerate() {
            let slot = ((write + i as u64) % CAPACITY as u64) as usize;
            unsafe { std::ptr::write_volatile(&mut ring.samples[slot], s) };
        }

        // Publish the new write position only after the samples are stored.
        fence(Ordering::Release);
        unsafe { std::ptr::write_volatile(&mut ring.write_index, write + n as u64) };
        n
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A test-only consumer mirroring the driver's read side.
    struct Consumer {
        ring: *mut Ring,
    }
    impl Consumer {
        fn pop(&mut self, out: &mut [i16]) -> usize {
            let ring = unsafe { &mut *self.ring };
            let write = unsafe { std::ptr::read_volatile(&ring.write_index) };
            fence(Ordering::Acquire);
            let read = ring.read_index;
            let avail = (write - read) as usize;
            let n = out.len().min(avail);
            for (i, slot_out) in out[..n].iter_mut().enumerate() {
                let slot = ((read + i as u64) % CAPACITY as u64) as usize;
                *slot_out = unsafe { std::ptr::read_volatile(&ring.samples[slot]) };
            }
            fence(Ordering::Release);
            unsafe { std::ptr::write_volatile(&mut ring.read_index, read + n as u64) };
            n
        }
    }

    fn fresh() -> Box<Ring> {
        // Zeroed ring, header initialised — like a freshly-mapped section.
        let mut r: Box<Ring> = unsafe { Box::new(std::mem::zeroed()) };
        r.init_header();
        r
    }

    #[test]
    fn layout_matches_c_header() {
        // Offsets the C side (ring.h) relies on. If these change, ring.h must too.
        assert_eq!(std::mem::size_of::<Ring>(), 192 + CAPACITY * 2);
        let base = fresh();
        let p = &*base as *const Ring as usize;
        assert_eq!(&base.write_index as *const _ as usize - p, 64);
        assert_eq!(&base.read_index as *const _ as usize - p, 128);
        assert_eq!(&base.samples as *const _ as usize - p, 192);
    }

    #[test]
    fn header_validation() {
        let mut r = fresh();
        assert!(r.header_ok());
        r.abi_version = 999;
        assert!(!r.header_ok());
    }

    #[test]
    fn push_pop_roundtrip() {
        let mut ring = fresh();
        let ptr = &mut *ring as *mut Ring;
        let mut prod = unsafe { Producer::new(ptr) };
        let mut cons = Consumer { ring: ptr };

        assert_eq!(prod.push(&[1, 2, 3, 4]), 4);
        assert_eq!(prod.occupied(), 4);
        let mut out = [0i16; 4];
        assert_eq!(cons.pop(&mut out), 4);
        assert_eq!(out, [1, 2, 3, 4]);
        assert_eq!(prod.occupied(), 0);
    }

    #[test]
    fn backpressure_when_full() {
        let mut ring = fresh();
        let ptr = &mut *ring as *mut Ring;
        let mut prod = unsafe { Producer::new(ptr) };

        let big = vec![7i16; CAPACITY + 100];
        // Only CAPACITY fit; the extra 100 are dropped, not blocked.
        assert_eq!(prod.push(&big), CAPACITY);
        assert_eq!(prod.vacant(), 0);
        assert_eq!(prod.push(&[1, 2, 3]), 0);
    }

    #[test]
    fn wraps_around_correctly() {
        let mut ring = fresh();
        let ptr = &mut *ring as *mut Ring;
        let mut prod = unsafe { Producer::new(ptr) };
        let mut cons = Consumer { ring: ptr };

        // Advance both indices near the end so the next write straddles the seam.
        let filler = vec![0i16; CAPACITY - 5];
        prod.push(&filler);
        let mut sink = vec![0i16; CAPACITY - 5];
        cons.pop(&mut sink);

        // Now write 10 samples across the wrap point and read them back in order.
        let data: Vec<i16> = (1..=10).collect();
        assert_eq!(prod.push(&data), 10);
        let mut out = [0i16; 10];
        assert_eq!(cons.pop(&mut out), 10);
        assert_eq!(&out[..], &data[..]);
    }
}
