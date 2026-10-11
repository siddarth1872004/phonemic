// Jitter buffer: reorder packets by sequence number and turn gaps into explicit
// conceal signals.
//
// A fixed ring of `capacity` slots indexed by `seq % capacity`. Two playout
// styles are supported:
//
//  * pop()        — timer-driven (one call per frame period). The buffer first
//                   *primes* until it holds `target_depth` frames, then each call
//                   advances the cursor by exactly one sequence number:
//                     slot present                 -> Frame
//                     slot missing, later queued   -> Conceal (packet lost)
//                     nothing queued               -> Empty (underrun, re-prime)
//  * pop_ready(n) — arrival-driven (call after every insert until Empty). Emits
//                   in-order frames immediately (no added latency) and declares a
//                   missing frame lost only once `n` later frames have arrived,
//                   i.e. it tolerates reordering by up to n packets. The desktop
//                   receiver uses this; smoothing of network jitter and clock
//                   drift happens downstream in the adaptive playout buffer.
//
// Late packets are dropped, duplicates ignored, and a large forward jump slides
// the window, dropping the stale oldest frames. Sequence numbers are per session
// (u32 at 100 packets/s lasts ~497 days), so wrap-around is not handled.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace phonemic {

class JitterBuffer {
public:
    enum class Insert { Buffered, Duplicate, TooLate, Overflowed };
    enum class PopKind { Frame, Conceal, Empty };

    struct Pop {
        PopKind kind = PopKind::Empty;
        std::vector<uint8_t> payload;  // only for Frame
    };

    struct Stats {
        uint64_t buffered = 0, duplicates = 0, too_late = 0, overflow_dropped = 0;
        uint64_t emitted = 0, concealed = 0, underruns = 0;
    };

    // Throws std::invalid_argument if capacity == 0 or target_depth > capacity.
    JitterBuffer(uint32_t capacity, uint32_t target_depth);

    Insert insert(uint32_t seq, std::vector<uint8_t> payload);
    // Number of frames dropped by the most recent Overflowed insert.
    uint32_t last_overflow_dropped() const { return last_overflow_dropped_; }

    Pop pop();
    Pop pop_ready(uint32_t reorder_depth);

    // Payload of the frame the cursor points at, if it has arrived (used to
    // recover a lost frame from the next packet's forward error correction).
    const std::vector<uint8_t>* peek_next() const;

    void reset();
    uint32_t len() const { return len_; }
    bool empty() const { return len_ == 0; }
    const Stats& stats() const { return stats_; }

private:
    size_t slot_of(uint32_t seq) const { return seq % capacity_; }
    std::optional<uint32_t> earliest_buffered() const;
    Pop take_next(uint32_t next);

    std::vector<std::optional<std::vector<uint8_t>>> slots_;
    uint32_t capacity_;
    uint32_t target_depth_;
    std::optional<uint32_t> next_seq_;
    uint32_t len_ = 0;
    bool primed_ = false;
    bool started_ = false;
    uint32_t last_overflow_dropped_ = 0;
    Stats stats_;
};

}  // namespace phonemic
