#include "phonemic/playout.hpp"

#include <algorithm>
#include <cmath>

namespace phonemic {

namespace {
constexpr size_t kStage = 256;
constexpr double kMaxCorrection = 0.004;  // +/-0.4 % read-rate adjustment
}  // namespace

AdaptivePlayout::AdaptivePlayout(uint32_t sample_rate, uint32_t capacity_ms)
    : rate_(sample_rate), ring_(static_cast<size_t>(sample_rate) * capacity_ms / 1000) {
    stage_.assign(kStage, 0.0f);
}

void AdaptivePlayout::set_target_ms(uint32_t ms) {
    target_ms_.store(std::clamp<uint32_t>(ms, 10, 500), std::memory_order_relaxed);
}

size_t AdaptivePlayout::write(const float* s, size_t n) {
    const size_t ok = ring_.push(s, n);
    if (ok < n) st_overflow_.fetch_add(n - ok, std::memory_order_relaxed);
    return ok;
}

size_t AdaptivePlayout::buffered() const { return ring_.size() + (stage_len_ - stage_pos_); }

bool AdaptivePlayout::refill_stage() {
    stage_len_ = ring_.pop(stage_.data(), kStage);
    stage_pos_ = 0;
    return stage_len_ > 0;
}

void AdaptivePlayout::render(float* out, size_t n) {
    if (flush_req_.exchange(false, std::memory_order_acq_rel)) {
        ring_.skip(ring_.size());
        stage_len_ = stage_pos_ = 0;
        running_ = false;
    }

    const double target = static_cast<double>(target_ms_.load(std::memory_order_relaxed)) *
                          rate_ / 1000.0;
    size_t fill = buffered();

    if (!running_) {
        if (static_cast<double>(fill) >= target) {
            running_ = true;
            fill_avg_ = static_cast<double>(fill);
            frac_ = 1.0;
            prev_ = cur_ = 0.0f;
        } else {
            std::fill(out, out + n, 0.0f);
            st_fill_ms_.store(static_cast<float>(fill * 1000.0 / rate_), std::memory_order_relaxed);
            st_playing_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    // Bound latency: if far over target (sender burst / paused consumer), drop
    // back to the target in one step instead of lagging for seconds.
    const double hard_max = target * 2.0 + rate_ * 0.06;
    if (static_cast<double>(fill) > hard_max) {
        const size_t excess = fill - static_cast<size_t>(target);
        const size_t from_stage = std::min(excess, stage_len_ - stage_pos_);
        stage_pos_ += from_stage;
        ring_.skip(excess - from_stage);
        fill = buffered();
        fill_avg_ = static_cast<double>(fill);
        st_resyncs_.fetch_add(1, std::memory_order_relaxed);
    }

    // Smoothed fill -> proportional rate correction.
    fill_avg_ += 0.02 * (static_cast<double>(fill) - fill_avg_);
    const double err = (fill_avg_ - target) / std::max(target, 1.0);
    const double ratio = 1.0 + std::clamp(err * 0.02, -kMaxCorrection, kMaxCorrection);

    size_t i = 0;
    for (; i < n; ++i) {
        while (frac_ >= 1.0) {
            if (stage_pos_ == stage_len_ && !refill_stage()) {
                // Underrun: play silence and re-prime to the target.
                running_ = false;
                st_underruns_.fetch_add(1, std::memory_order_relaxed);
                // Short fade from the last sample avoids a click.
                float last = cur_;
                for (; i < n; ++i) {
                    last *= 0.9f;
                    out[i] = last;
                }
                goto done;
            }
            prev_ = cur_;
            cur_ = stage_[stage_pos_++];
            frac_ -= 1.0;
        }
        out[i] = prev_ + (cur_ - prev_) * static_cast<float>(frac_);
        frac_ += ratio;
    }
done:
    st_fill_ms_.store(static_cast<float>(buffered() * 1000.0 / rate_), std::memory_order_relaxed);
    st_ratio_.store(static_cast<float>(ratio), std::memory_order_relaxed);
    st_playing_.store(running_, std::memory_order_relaxed);
}

AdaptivePlayout::Stats AdaptivePlayout::stats() const {
    Stats s;
    s.fill_ms = st_fill_ms_.load(std::memory_order_relaxed);
    s.ratio = st_ratio_.load(std::memory_order_relaxed);
    s.underruns = st_underruns_.load(std::memory_order_relaxed);
    s.resyncs = st_resyncs_.load(std::memory_order_relaxed);
    s.overflow_samples = st_overflow_.load(std::memory_order_relaxed);
    s.playing = st_playing_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace phonemic
