// Jitter buffer tests (all ported from the Rust suite, plus arrival-driven mode).
#include "phonemic/jitter_buffer.hpp"

#include <optional>
#include <vector>

#include "test.hpp"

using phonemic::JitterBuffer;
using Insert = JitterBuffer::Insert;
using PopKind = JitterBuffer::PopKind;

namespace {
std::vector<uint8_t> payload(uint8_t tag) { return std::vector<uint8_t>(4, tag); }

// Drain and collect first bytes (nullopt = conceal), stopping at the first Empty.
std::vector<std::optional<uint8_t>> drain(JitterBuffer& jb, int steps, bool ready = false, uint32_t depth = 2) {
    std::vector<std::optional<uint8_t>> out;
    for (int i = 0; i < steps; ++i) {
        auto p = ready ? jb.pop_ready(depth) : jb.pop();
        if (p.kind == PopKind::Empty) break;
        if (p.kind == PopKind::Frame) out.push_back(p.payload[0]);
        else out.push_back(std::nullopt);
    }
    return out;
}
using V = std::vector<std::optional<uint8_t>>;
}  // namespace

TEST(jb_in_order_after_priming) {
    JitterBuffer jb(8, 2);
    CHECK(jb.pop().kind == PopKind::Empty);
    jb.insert(0, payload(0));
    CHECK(jb.pop().kind == PopKind::Empty);
    jb.insert(1, payload(1));
    jb.insert(2, payload(2));
    CHECK(drain(jb, 3) == (V{0, 1, 2}));
}

TEST(jb_reorders_out_of_order_arrivals) {
    JitterBuffer jb(8, 3);
    jb.insert(2, payload(2));
    jb.insert(0, payload(0));
    jb.insert(1, payload(1));
    CHECK(drain(jb, 3) == (V{0, 1, 2}));
}

TEST(jb_conceals_a_lost_packet) {
    JitterBuffer jb(8, 3);
    jb.insert(0, payload(0));
    jb.insert(1, payload(1));
    jb.insert(3, payload(3));
    CHECK(drain(jb, 4) == (V{0, 1, std::nullopt, 3}));
    CHECK_EQ(jb.stats().concealed, 1u);
    CHECK_EQ(jb.stats().emitted, 3u);
}

TEST(jb_drops_late_packet) {
    JitterBuffer jb(8, 1);
    jb.insert(0, payload(0));
    jb.insert(1, payload(1));
    CHECK(jb.pop().payload == payload(0));
    CHECK(jb.pop().payload == payload(1));
    CHECK(jb.insert(0, payload(0)) == Insert::TooLate);
    CHECK_EQ(jb.stats().too_late, 1u);
}

TEST(jb_ignores_duplicate) {
    JitterBuffer jb(8, 2);
    CHECK(jb.insert(5, payload(5)) == Insert::Buffered);
    CHECK(jb.insert(5, payload(5)) == Insert::Duplicate);
    CHECK_EQ(jb.len(), 1u);
    CHECK_EQ(jb.stats().duplicates, 1u);
}

TEST(jb_underrun_then_recovers) {
    JitterBuffer jb(8, 1);
    jb.insert(0, payload(0));
    CHECK(jb.pop().payload == payload(0));
    CHECK(jb.pop().kind == PopKind::Empty);
    CHECK_EQ(jb.stats().underruns, 1u);
    jb.insert(1, payload(1));
    CHECK(jb.pop().payload == payload(1));
}

TEST(jb_forward_jump_slides_window_and_drops_stale) {
    JitterBuffer jb(4, 1);
    jb.insert(0, payload(0));
    jb.insert(1, payload(1));
    CHECK(jb.insert(7, payload(7)) == Insert::Overflowed);
    CHECK_EQ(jb.last_overflow_dropped(), 2u);
    CHECK_EQ(jb.stats().overflow_dropped, 2u);
    CHECK(jb.pop().payload == payload(7));
}

TEST(jb_does_not_emit_before_target_depth) {
    JitterBuffer jb(8, 4);
    for (uint8_t s = 0; s < 3; ++s) jb.insert(s, payload(s));
    CHECK(jb.pop().kind == PopKind::Empty);
    jb.insert(3, payload(3));
    CHECK(drain(jb, 4) == (V{0, 1, 2, 3}));
}

TEST(jb_zero_target_depth_emits_immediately) {
    JitterBuffer jb(4, 0);
    jb.insert(0, payload(0));
    CHECK(jb.pop().payload == payload(0));
}

TEST(jb_full_capacity_window_all_present) {
    JitterBuffer jb(4, 4);
    for (uint8_t s = 0; s < 4; ++s) jb.insert(s, payload(s));
    CHECK_EQ(jb.len(), 4u);
    CHECK(drain(jb, 4) == (V{0, 1, 2, 3}));
}

TEST(jb_rejects_bad_construction) {
    CHECK_THROWS(JitterBuffer(2, 3));
    CHECK_THROWS(JitterBuffer(0, 0));
}

TEST(jb_ready_mode_passes_in_order_without_delay) {
    JitterBuffer jb(16, 0);
    for (uint8_t s = 10; s < 15; ++s) {
        jb.insert(s, payload(s));
        CHECK(drain(jb, 10, true) == (V{s}));
    }
}

TEST(jb_ready_mode_tolerates_reorder_within_depth) {
    JitterBuffer jb(16, 0);
    jb.insert(0, payload(0));
    CHECK(drain(jb, 5, true) == (V{0}));
    jb.insert(2, payload(2));  // 1 is late but may still come
    CHECK(drain(jb, 5, true).empty());
    jb.insert(1, payload(1));
    CHECK(drain(jb, 5, true) == (V{1, 2}));
    CHECK_EQ(jb.stats().concealed, 0u);
}

TEST(jb_ready_mode_declares_loss_after_depth) {
    JitterBuffer jb(16, 0);
    jb.insert(0, payload(0));
    drain(jb, 5, true);
    jb.insert(2, payload(2));
    CHECK(drain(jb, 5, true).empty());
    jb.insert(3, payload(3));  // two frames past the gap -> 1 is lost
    CHECK(drain(jb, 5, true) == (V{std::nullopt, 2, 3}));
    CHECK_EQ(jb.stats().concealed, 1u);
}

TEST(jb_reset_clears_everything) {
    JitterBuffer jb(8, 0);
    jb.insert(100, payload(1));
    jb.reset();
    CHECK(jb.empty());
    jb.insert(3, payload(3));
    CHECK(drain(jb, 3, true) == (V{3}));
}
