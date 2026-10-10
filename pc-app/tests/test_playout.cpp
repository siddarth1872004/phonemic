#include "phonemic/playout.hpp"

#include <cmath>
#include <vector>

#include "test.hpp"

using phonemic::AdaptivePlayout;

namespace {
// Simulate a sender whose clock runs `ppm` fast/slow against the sound card for
// `seconds`, in 1 ms steps: 10 ms packets in, 256-sample device callbacks out.
struct Sim {
    AdaptivePlayout po{48000, 1000};
    double in_acc = 0, out_acc = 0;
    std::vector<float> block = std::vector<float>(480, 0.25f), out = std::vector<float>(256);
    float min_fill = 1e9f, max_fill = 0;
    void run(double ppm, double seconds, double measure_after) {
        const double in_rate = 48000.0 * (1.0 + ppm * 1e-6);
        for (int ms = 0; ms < seconds * 1000; ++ms) {
            in_acc += in_rate / 1000.0;
            while (in_acc >= 480) { po.write(block.data(), 480); in_acc -= 480; }
            out_acc += 48.0;
            while (out_acc >= 256) {
                po.render(out.data(), 256);
                out_acc -= 256;
                if (ms > measure_after * 1000) {
                    const float f = po.stats().fill_ms;
                    min_fill = std::min(min_fill, f);
                    max_fill = std::max(max_fill, f);
                }
            }
        }
    }
};
}  // namespace

TEST(playout_holds_latency_with_fast_sender_clock) {
    Sim s;
    s.po.set_target_ms(40);
    s.run(+300.0, 180.0, 60.0);  // a very bad (300 ppm) clock, 3 minutes
    const auto st = s.po.stats();
    CHECK_EQ(st.underruns, 0u);
    CHECK_EQ(st.resyncs, 0u);
    CHECK(s.max_fill < 60.0f);  // a plain FIFO would have grown by ~54 ms by now
    CHECK(st.ratio > 1.0f);     // consuming slightly faster to compensate
}

TEST(playout_holds_latency_with_slow_sender_clock) {
    Sim s;
    s.po.set_target_ms(40);
    s.run(-300.0, 180.0, 60.0);
    const auto st = s.po.stats();
    CHECK_EQ(st.underruns, 0u);  // a plain FIFO would underrun every ~3 minutes
    CHECK(s.min_fill > 15.0f);
    CHECK(st.ratio < 1.0f);
}

TEST(playout_primes_before_playing_and_reprimes_after_underrun) {
    AdaptivePlayout po(48000, 1000);
    po.set_target_ms(20);
    std::vector<float> in(480, 0.5f), out(480);
    po.write(in.data(), 480);  // 10 ms < 20 ms target
    po.render(out.data(), 480);
    CHECK(!po.stats().playing);
    for (float x : out) CHECK_EQ(x, 0.0f);
    po.write(in.data(), 480);
    po.write(in.data(), 480);
    po.render(out.data(), 480);
    CHECK(po.stats().playing);
    CHECK_NEAR(out[200], 0.5, 1e-3);
    for (int i = 0; i < 4; ++i) po.render(out.data(), 480);  // starve it
    CHECK_EQ(po.stats().underruns, 1u);
    CHECK(!po.stats().playing);
}

TEST(playout_bounds_latency_after_a_burst) {
    AdaptivePlayout po(48000, 1000);
    po.set_target_ms(40);
    std::vector<float> in(48000 * 2 / 5, 0.1f), out(256);  // 400 ms arrives at once
    po.write(in.data(), in.size());
    po.render(out.data(), 256);
    CHECK_EQ(po.stats().resyncs, 1u);
    CHECK(po.stats().fill_ms < 45.0f);
}

TEST(playout_flush_drops_audio_and_reprimes) {
    AdaptivePlayout po(48000, 1000);
    po.set_target_ms(10);
    std::vector<float> in(4800, 0.3f), out(480);
    po.write(in.data(), in.size());
    po.render(out.data(), 480);
    CHECK(po.stats().playing);
    po.request_flush();
    po.render(out.data(), 480);
    CHECK(!po.stats().playing);
    CHECK_NEAR(po.stats().fill_ms, 0.0, 1e-6);
}

TEST(playout_overflow_is_counted_not_blocking) {
    AdaptivePlayout po(48000, 100);  // ~100 ms ring (rounded up to a power of two)
    std::vector<float> in(48000, 0.0f);
    const size_t ok = po.write(in.data(), in.size());
    CHECK(ok < in.size());
    CHECK_EQ(po.stats().overflow_samples, in.size() - ok);
}
