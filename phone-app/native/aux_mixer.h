// Soundboard audio mixed into the outgoing mic stream.
//
// The soundboard's decode thread pushes PCM (already downmixed and resampled
// to the capture rate) into AuxRing; the sender thread pops one 10 ms frame's
// worth and AuxMixer adds it to the mic frame before Opus. The Oboe callback
// never touches any of this. Kept free of Android headers so the host test
// suite (pc-app/tests) can exercise it.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phonemic {

// Single producer / single consumer. The producer is AudioEngine's aux_* API
// (serialised by its own mutex), the consumer the sender thread.
class AuxRing {
public:
    // Bounds how far a sound can be queued ahead of the mic (~680 ms @ 48 kHz);
    // the producer waits for room instead of growing it.
    static constexpr size_t kSize = 1 << 15;

    AuxRing() : buf_(kSize, 0) {}

    // Producer. May under-report right after discard() until the consumer
    // has skipped the discarded samples; the producer just retries.
    size_t room() const {
        return kSize - static_cast<size_t>(write_.load(std::memory_order_relaxed) -
                                           read_.load(std::memory_order_acquire));
    }
    // Producer. n must not exceed room().
    void push(const int16_t* s, size_t n) {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & (kSize - 1)] = s[i];
        write_.store(w + n, std::memory_order_release);
    }
    // Producer. Everything pushed so far is skipped by the consumer, which is
    // the only side allowed to move read_.
    void discard() { discard_to_.store(write_.load(std::memory_order_relaxed), std::memory_order_release); }

    // Consumer. Returns how many samples were copied (0..n).
    size_t pop(int16_t* out, size_t n) {
        uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t d = discard_to_.load(std::memory_order_acquire);
        if (d > r) r = d;
        const uint64_t avail = write_.load(std::memory_order_acquire) - r;
        const size_t k = avail < n ? static_cast<size_t>(avail) : n;
        for (size_t i = 0; i < k; ++i) out[i] = buf_[(r + i) & (kSize - 1)];
        read_.store(r + k, std::memory_order_release);
        return k;
    }
    // Consumer side, while no consumer is running: forget anything queued so a
    // new stream never starts with a previous session's leftovers.
    void drop_all() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<int16_t> buf_;
    std::atomic<uint64_t> write_{0}, read_{0}, discard_to_{0};
};

// Producer-side downmix + linear-interpolation resampler, streaming across
// chunks of one sound. begin() starts a new sound.
class AuxResampler {
public:
    static bool valid(int src_rate, int channels) {
        return src_rate >= 8000 && src_rate <= 192000 && channels >= 1 && channels <= 8;
    }

    void begin(int src_rate, int channels, int dst_rate) {
        channels_ = channels;
        step_ = static_cast<double>(src_rate) / static_cast<double>(dst_rate);
        // Worst case outputs per input sample, so a chunk never overruns the ring.
        max_out_per_in_ = static_cast<size_t>(std::ceil(1.0 / step_)) + 1;
        t_ = 0.0;
        have_prev_ = false;
        prev_ = 0.0f;
    }

    // Consume interleaved frames while the ring has room. Returns the number
    // of input frames consumed; the caller re-offers the rest later.
    size_t process(const int16_t* in, size_t frames, AuxRing& ring) {
        size_t budget = ring.room();
        out_.clear();
        size_t used = 0;
        for (; used < frames && budget >= max_out_per_in_; ++used) {
            int32_t sum = 0;
            for (int c = 0; c < channels_; ++c) sum += in[used * static_cast<size_t>(channels_) + static_cast<size_t>(c)];
            const float x = static_cast<float>(sum) / static_cast<float>(channels_);
            if (!have_prev_) {
                prev_ = x;
                have_prev_ = true;
                continue;
            }
            while (t_ < 1.0) {
                const float y = prev_ + (x - prev_) * static_cast<float>(t_);
                out_.push_back(static_cast<int16_t>(std::lround(y)));
                t_ += step_;
                --budget;
            }
            t_ -= 1.0;
            prev_ = x;
        }
        if (!out_.empty()) ring.push(out_.data(), out_.size());
        return used;
    }

private:
    int channels_ = 1;
    double step_ = 1.0, t_ = 0.0;
    size_t max_out_per_in_ = 2;
    bool have_prev_ = false;
    float prev_ = 0.0f;
    std::vector<int16_t> out_;
};

// Consumer side: mixed = mic + aux * gain, then a peak limiter so the sum of
// a loud voice and a loud clip never wraps or hard-clips.
class AuxMixer {
public:
    static constexpr float kDefaultGain = 0.5f;  // -6 dB: soundboard clips are mastered hot

    void set_gain(float g) { gain_.store(std::clamp(g, 0.0f, 2.0f), std::memory_order_relaxed); }
    float gain() const { return gain_.load(std::memory_order_relaxed); }
    void reset() { env_ = 1.0f; }

    // Mixes up to n queued aux samples into pcm in place. With nothing queued
    // and the limiter at rest the mic frame is left bit-for-bit untouched.
    // When muted the aux samples are still consumed, so they do not pile up
    // and play late after unmuting. Returns how many aux samples were mixed.
    size_t mix(int16_t* pcm, size_t n, AuxRing& ring, bool muted) {
        if (aux_.size() < n) aux_.resize(n);
        const size_t got = ring.pop(aux_.data(), n);
        if (muted) return 0;
        if (got == 0 && env_ >= 1.0f) return 0;

        const float g = gain();
        if (mixed_.size() < n) mixed_.resize(n);
        float peak = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const float m = static_cast<float>(pcm[i]) + (i < got ? static_cast<float>(aux_[i]) * g : 0.0f);
            mixed_[i] = m;
            peak = std::max(peak, std::fabs(m));
        }
        // The release is capped by this frame's target, so no sample in the
        // frame exceeds the threshold.
        const float target = peak > kThreshold ? kThreshold / peak : 1.0f;
        if (target < env_) env_ = target;
        for (size_t i = 0; i < n; ++i) {
            const float y = mixed_[i] * env_;
            pcm[i] = static_cast<int16_t>(std::clamp(std::lround(y), -32768L, 32767L));
            env_ = std::min(target, env_ + (1.0f - env_) * kRelease);
        }
        if (env_ > 0.9999f) env_ = 1.0f;
        return got;
    }

private:
    static constexpr float kThreshold = 0.9f * 32767.0f;
    static constexpr float kRelease = 1.0f / 2400.0f;  // ~50 ms at 48 kHz

    std::atomic<float> gain_{kDefaultGain};
    float env_ = 1.0f;
    std::vector<int16_t> aux_;
    std::vector<float> mixed_;
};

}  // namespace phonemic
