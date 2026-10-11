#include "phonemic/jitter_buffer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace phonemic {

JitterBuffer::JitterBuffer(uint32_t capacity, uint32_t target_depth)
    : capacity_(capacity), target_depth_(target_depth) {
    if (capacity == 0) throw std::invalid_argument("capacity must be > 0");
    if (target_depth > capacity)
        throw std::invalid_argument("target_depth (" + std::to_string(target_depth) +
                                    ") must be <= capacity (" + std::to_string(capacity) + ")");
    slots_.resize(capacity);
}

void JitterBuffer::reset() {
    for (auto& s : slots_) s.reset();
    next_seq_.reset();
    len_ = 0;
    primed_ = false;
    started_ = false;
}

const std::vector<uint8_t>* JitterBuffer::peek_next() const {
    if (!next_seq_) return nullptr;
    const auto& slot = slots_[slot_of(*next_seq_)];
    return slot ? &*slot : nullptr;
}

std::optional<uint32_t> JitterBuffer::earliest_buffered() const {
    if (!next_seq_) return std::nullopt;
    for (uint32_t i = 0; i < capacity_; ++i) {
        const uint32_t seq = *next_seq_ + i;
        if (slots_[slot_of(seq)]) return seq;
    }
    return std::nullopt;
}

JitterBuffer::Insert JitterBuffer::insert(uint32_t seq, std::vector<uint8_t> payload) {
    last_overflow_dropped_ = 0;
    uint32_t next;
    if (!next_seq_) {
        // First packet ever defines where the play cursor starts.
        next_seq_ = seq;
        next = seq;
    } else if (!started_ && seq < *next_seq_ && (*next_seq_ - seq) < capacity_) {
        // Before playout begins, an earlier packet is a reorder, not a late
        // arrival: pull the cursor back to include it.
        next_seq_ = seq;
        next = seq;
    } else {
        next = *next_seq_;
    }

    if (seq < next) {
        ++stats_.too_late;
        return Insert::TooLate;
    }

    // Too far ahead to fit: slide the window forward, dropping stale frames.
    uint32_t dropped = 0;
    if (static_cast<uint64_t>(seq) >= static_cast<uint64_t>(next) + capacity_) {
        const uint32_t new_next = seq - capacity_ + 1;
        const uint32_t shift = std::min(new_next - next, capacity_);
        for (uint32_t i = 0; i < shift; ++i) {
            auto& slot = slots_[slot_of(next + i)];
            if (slot) {
                slot.reset();
                --len_;
                ++dropped;
            }
        }
        next_seq_ = new_next;
        stats_.overflow_dropped += dropped;
    }

    auto& slot = slots_[slot_of(seq)];
    if (slot) {
        // Within the live window each seq maps to a unique slot.
        ++stats_.duplicates;
        return Insert::Duplicate;
    }
    slot = std::move(payload);
    ++len_;
    ++stats_.buffered;

    if (dropped > 0) {
        // Catch the cursor up to the earliest frame still held instead of
        // concealing phantom gaps for audio that is already gone.
        next_seq_ = earliest_buffered();
        last_overflow_dropped_ = dropped;
        return Insert::Overflowed;
    }
    return Insert::Buffered;
}

JitterBuffer::Pop JitterBuffer::take_next(uint32_t next) {
    Pop out;
    auto& slot = slots_[slot_of(next)];
    out.kind = PopKind::Frame;
    out.payload = std::move(*slot);
    slot.reset();
    --len_;
    next_seq_ = next + 1;
    started_ = true;
    ++stats_.emitted;
    return out;
}

JitterBuffer::Pop JitterBuffer::pop() {
    if (!next_seq_) return {};
    const uint32_t next = *next_seq_;

    if (!primed_) {
        if (len_ >= target_depth_ && len_ > 0) {
            primed_ = true;
        } else {
            return {};
        }
    }

    if (slots_[slot_of(next)]) return take_next(next);
    if (len_ > 0) {
        next_seq_ = next + 1;
        started_ = true;
        ++stats_.concealed;
        Pop p;
        p.kind = PopKind::Conceal;
        return p;
    }
    primed_ = false;
    ++stats_.underruns;
    return {};
}

JitterBuffer::Pop JitterBuffer::pop_ready(uint32_t reorder_depth) {
    if (!next_seq_) return {};
    const uint32_t next = *next_seq_;
    if (slots_[slot_of(next)]) return take_next(next);
    if (len_ > 0 && len_ >= std::max<uint32_t>(reorder_depth, 1)) {
        // Enough later frames have arrived that `next` is considered lost.
        next_seq_ = next + 1;
        started_ = true;
        ++stats_.concealed;
        Pop p;
        p.kind = PopKind::Conceal;
        return p;
    }
    return {};
}

}  // namespace phonemic
