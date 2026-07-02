//! Jitter buffer: reorder packets by sequence number and smooth network jitter
//! before the decoder, turning gaps into explicit conceal signals.
//!
//! This is the single most bug-prone piece of the PC side — the failure mode is
//! *silent audio corruption*, not a crash — so it lives here as ordinary,
//! heavily-tested library code.
//!
//! # Model
//!
//! A fixed-size ring of `capacity` slots indexed by `seq % capacity`. The buffer
//! initially *primes* — accepting packets without emitting — until it holds
//! `target_depth` frames, which is what absorbs jitter. After that, each [`pop`]
//! advances the play cursor by exactly one sequence number:
//!
//! - slot present  → [`Pop::Frame`] with the payload.
//! - slot missing but later frames are buffered → [`Pop::Conceal`]: the packet
//!   was lost; the caller runs Opus packet-loss concealment (or emits silence in
//!   the Phase 0 PCM path).
//! - slot missing and nothing buffered ahead → [`Pop::Empty`]: underrun. The
//!   cursor does not advance and the buffer re-primes.
//!
//! Late packets (`seq` already played past) are dropped; duplicates are ignored;
//! a large forward jump slides the window, dropping the stale oldest frames.
//!
//! Sequence numbers are assumed not to wrap within a session (a `u32` at ~100
//! packets/s lasts ~1.3 years). This is documented rather than handled.
//!
//! [`pop`]: JitterBuffer::pop

/// Outcome of inserting a packet — surfaced mainly so tests and stats can see
/// what the buffer decided.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Insert {
    /// Stored in its slot, awaiting playout.
    Buffered,
    /// A packet for this `seq` was already buffered; ignored.
    Duplicate,
    /// `seq` was already played past; dropped.
    TooLate,
    /// `seq` was so far ahead it slid the window; `dropped` old frames were lost.
    Overflowed { dropped: u32 },
}

/// Outcome of a playout step.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Pop {
    /// A frame is ready. Payload is the bytes handed to [`JitterBuffer::insert`].
    Frame(Vec<u8>),
    /// The next frame was lost; run packet-loss concealment.
    Conceal,
    /// Nothing to play yet (still priming, or an underrun). No cursor advance.
    Empty,
}

/// Running counters, handy for the latency/quality report at the end of a phase.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Stats {
    pub buffered: u64,
    pub duplicates: u64,
    pub too_late: u64,
    pub overflow_dropped: u64,
    pub emitted: u64,
    pub concealed: u64,
    pub underruns: u64,
}

/// A fixed-capacity reorder + jitter-smoothing buffer.
pub struct JitterBuffer {
    slots: Vec<Option<Vec<u8>>>,
    capacity: u32,
    target_depth: u32,
    /// Sequence number of the next frame to emit. `None` until the first insert.
    next_seq: Option<u32>,
    /// Number of occupied slots.
    len: u32,
    /// Whether we've reached `target_depth` and are actively emitting.
    primed: bool,
    /// Whether playout has ever begun. Before it has, an earlier-than-cursor
    /// packet is a legitimate reorder and pulls the cursor back; after it has,
    /// such a packet is genuinely too late.
    started: bool,
    stats: Stats,
}

impl JitterBuffer {
    /// Create a buffer holding up to `capacity` frames, emitting only once
    /// `target_depth` frames are queued.
    ///
    /// # Panics
    /// If `capacity == 0` or `target_depth > capacity` — both are programmer
    /// errors that would make the buffer nonsensical.
    pub fn new(capacity: u32, target_depth: u32) -> Self {
        assert!(capacity > 0, "capacity must be > 0");
        assert!(
            target_depth <= capacity,
            "target_depth ({target_depth}) must be <= capacity ({capacity})"
        );
        JitterBuffer {
            slots: (0..capacity).map(|_| None).collect(),
            capacity,
            target_depth,
            next_seq: None,
            len: 0,
            primed: false,
            started: false,
            stats: Stats::default(),
        }
    }

    /// Number of frames currently queued.
    pub fn len(&self) -> u32 {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn stats(&self) -> Stats {
        self.stats
    }

    #[inline]
    fn slot_of(&self, seq: u32) -> usize {
        (seq % self.capacity) as usize
    }

    /// Lowest sequence number currently buffered at or after the play cursor,
    /// or `None` if the buffer is empty. O(capacity); only used on the rare
    /// overflow path.
    fn earliest_buffered(&self) -> Option<u32> {
        let base = self.next_seq?;
        (0..self.capacity)
            .map(|i| base + i)
            .find(|&seq| self.slots[self.slot_of(seq)].is_some())
    }

    /// Insert a received packet's payload under its sequence number.
    pub fn insert(&mut self, seq: u32, payload: Vec<u8>) -> Insert {
        let next = match self.next_seq {
            // First packet ever defines where the play cursor starts.
            None => {
                self.next_seq = Some(seq);
                seq
            }
            // Before playout begins, a packet earlier than the cursor is a
            // reorder, not a late arrival: pull the cursor back to include it
            // (as long as it still fits the capacity window alongside what's
            // already buffered).
            Some(n) if !self.started && seq < n && (n - seq) < self.capacity => {
                self.next_seq = Some(seq);
                seq
            }
            Some(n) => n,
        };

        // Already played past this sequence number (or too far behind to fit).
        if seq < next {
            self.stats.too_late += 1;
            return Insert::TooLate;
        }

        // Too far ahead to fit: slide the window forward, dropping stale frames.
        let mut overflow_dropped = 0;
        if seq >= next.wrapping_add(self.capacity) {
            let new_next = seq - self.capacity + 1;
            // At most `capacity` slots can actually be occupied, so cap the
            // clearing loop instead of walking a potentially huge gap.
            let shift = (new_next - next).min(self.capacity);
            for i in 0..shift {
                let idx = self.slot_of(next + i);
                if self.slots[idx].take().is_some() {
                    self.len -= 1;
                    overflow_dropped += 1;
                }
            }
            self.next_seq = Some(new_next);
            self.stats.overflow_dropped += overflow_dropped as u64;
        }

        let idx = self.slot_of(seq);
        if self.slots[idx].is_some() {
            // Within the live window each seq maps to a unique slot, so an
            // occupied slot here means a duplicate of this exact seq.
            self.stats.duplicates += 1;
            return Insert::Duplicate;
        }
        self.slots[idx] = Some(payload);
        self.len += 1;
        self.stats.buffered += 1;

        if overflow_dropped > 0 {
            // The slide dropped the stale oldest frames, so the cursor may now
            // point at empty slots. Rather than conceal those phantom gaps
            // (added latency for audio that is gone), catch the cursor up to the
            // earliest frame we still hold. Small in-window gaps are still
            // concealed normally; only overflow (we're a full window behind)
            // triggers this skip.
            self.next_seq = self.earliest_buffered();
            Insert::Overflowed {
                dropped: overflow_dropped,
            }
        } else {
            Insert::Buffered
        }
    }

    /// Advance playout by one step. See [`Pop`] for the three outcomes.
    pub fn pop(&mut self) -> Pop {
        let next = match self.next_seq {
            None => return Pop::Empty, // nothing ever inserted
            Some(n) => n,
        };

        // Prime: wait until we've buffered enough to absorb jitter before we
        // start draining. Re-primes after an underrun empties the buffer.
        if !self.primed {
            if self.len >= self.target_depth && self.len > 0 {
                self.primed = true;
            } else {
                return Pop::Empty;
            }
        }

        let idx = self.slot_of(next);
        if let Some(payload) = self.slots[idx].take() {
            self.len -= 1;
            self.next_seq = Some(next.wrapping_add(1));
            self.started = true;
            self.stats.emitted += 1;
            Pop::Frame(payload)
        } else if self.len > 0 {
            // The next frame is missing but later ones are queued → genuine loss.
            self.next_seq = Some(next.wrapping_add(1));
            self.started = true;
            self.stats.concealed += 1;
            Pop::Conceal
        } else {
            // Nothing buffered at all → underrun; hold the cursor and re-prime.
            self.primed = false;
            self.stats.underruns += 1;
            Pop::Empty
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn payload(tag: u8) -> Vec<u8> {
        vec![tag; 4]
    }

    /// Drain and collect the payloads' first byte, stopping at the first Empty.
    fn drain_tags(jb: &mut JitterBuffer, steps: usize) -> Vec<Option<u8>> {
        let mut out = Vec::new();
        for _ in 0..steps {
            match jb.pop() {
                Pop::Frame(p) => out.push(Some(p[0])),
                Pop::Conceal => out.push(None),
                Pop::Empty => break,
            }
        }
        out
    }

    #[test]
    fn in_order_after_priming() {
        let mut jb = JitterBuffer::new(8, 2);
        assert_eq!(jb.pop(), Pop::Empty); // nothing inserted yet
        jb.insert(0, payload(0));
        assert_eq!(jb.pop(), Pop::Empty); // below target_depth, still priming
        jb.insert(1, payload(1));
        jb.insert(2, payload(2));
        assert_eq!(drain_tags(&mut jb, 3), vec![Some(0), Some(1), Some(2)]);
    }

    #[test]
    fn reorders_out_of_order_arrivals() {
        let mut jb = JitterBuffer::new(8, 3);
        // Arrive 2, 0, 1 — should play 0, 1, 2.
        jb.insert(2, payload(2));
        jb.insert(0, payload(0));
        jb.insert(1, payload(1));
        assert_eq!(drain_tags(&mut jb, 3), vec![Some(0), Some(1), Some(2)]);
    }

    #[test]
    fn conceals_a_lost_packet() {
        let mut jb = JitterBuffer::new(8, 3);
        // seq 2 never arrives.
        jb.insert(0, payload(0));
        jb.insert(1, payload(1));
        jb.insert(3, payload(3));
        // 0, 1, then conceal for the missing 2, then 3.
        assert_eq!(
            drain_tags(&mut jb, 4),
            vec![Some(0), Some(1), None, Some(3)]
        );
        assert_eq!(jb.stats().concealed, 1);
        assert_eq!(jb.stats().emitted, 3);
    }

    #[test]
    fn drops_late_packet() {
        let mut jb = JitterBuffer::new(8, 1);
        jb.insert(0, payload(0));
        jb.insert(1, payload(1));
        assert_eq!(jb.pop(), Pop::Frame(payload(0)));
        assert_eq!(jb.pop(), Pop::Frame(payload(1)));
        // seq 0 shows up late — cursor is already at 2.
        assert_eq!(jb.insert(0, payload(0)), Insert::TooLate);
        assert_eq!(jb.stats().too_late, 1);
    }

    #[test]
    fn ignores_duplicate() {
        let mut jb = JitterBuffer::new(8, 2);
        assert_eq!(jb.insert(5, payload(5)), Insert::Buffered);
        assert_eq!(jb.insert(5, payload(5)), Insert::Duplicate);
        assert_eq!(jb.len(), 1);
        assert_eq!(jb.stats().duplicates, 1);
    }

    #[test]
    fn underrun_then_recovers() {
        let mut jb = JitterBuffer::new(8, 1);
        jb.insert(0, payload(0));
        assert_eq!(jb.pop(), Pop::Frame(payload(0)));
        // Buffer empty now → underrun, cursor holds at seq 1.
        assert_eq!(jb.pop(), Pop::Empty);
        assert_eq!(jb.stats().underruns, 1);
        // The delayed seq-1 arrives and plays.
        jb.insert(1, payload(1));
        assert_eq!(jb.pop(), Pop::Frame(payload(1)));
    }

    #[test]
    fn forward_jump_slides_window_and_drops_stale() {
        let mut jb = JitterBuffer::new(4, 1);
        jb.insert(0, payload(0));
        jb.insert(1, payload(1));
        // Jump way ahead: capacity 4, so window becomes [7-4+1 .. ] = [4..8).
        // seq 0 and 1 fall out and are dropped.
        match jb.insert(7, payload(7)) {
            Insert::Overflowed { dropped } => assert_eq!(dropped, 2),
            other => panic!("expected Overflowed, got {other:?}"),
        }
        assert_eq!(jb.stats().overflow_dropped, 2);
        // Next playable frame is the one we jumped to.
        assert_eq!(jb.pop(), Pop::Frame(payload(7)));
    }

    #[test]
    fn does_not_emit_before_target_depth() {
        let mut jb = JitterBuffer::new(8, 4);
        for s in 0..3 {
            jb.insert(s, payload(s as u8));
        }
        // Only 3 buffered, target is 4 → still priming.
        assert_eq!(jb.pop(), Pop::Empty);
        jb.insert(3, payload(3));
        // Now primed; drains all four in order.
        assert_eq!(
            drain_tags(&mut jb, 4),
            vec![Some(0), Some(1), Some(2), Some(3)]
        );
    }

    #[test]
    fn zero_target_depth_emits_immediately() {
        let mut jb = JitterBuffer::new(4, 0);
        jb.insert(0, payload(0));
        assert_eq!(jb.pop(), Pop::Frame(payload(0)));
    }

    #[test]
    fn full_capacity_window_all_present() {
        let mut jb = JitterBuffer::new(4, 4);
        for s in 0..4 {
            jb.insert(s, payload(s as u8));
        }
        assert_eq!(jb.len(), 4);
        assert_eq!(
            drain_tags(&mut jb, 4),
            vec![Some(0), Some(1), Some(2), Some(3)]
        );
    }

    #[test]
    #[should_panic]
    fn rejects_target_depth_over_capacity() {
        JitterBuffer::new(2, 3);
    }

    #[test]
    #[should_panic]
    fn rejects_zero_capacity() {
        JitterBuffer::new(0, 0);
    }
}
