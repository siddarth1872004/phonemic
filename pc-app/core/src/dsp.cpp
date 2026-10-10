#include "phonemic/dsp.hpp"

#include <algorithm>
#include <cmath>

namespace phonemic {

void pcm16le_to_float(const uint8_t* b, size_t samples, float* out) {
    for (size_t i = 0; i < samples; ++i) {
        const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(b[2 * i]) |
                                               (static_cast<uint16_t>(b[2 * i + 1]) << 8));
        out[i] = static_cast<float>(v) / 32768.0f;
    }
}

void float_to_pcm16le(const float* in, size_t samples, uint8_t* out) {
    for (size_t i = 0; i < samples; ++i) {
        float x = in[i] * 32768.0f;
        x = std::clamp(x, -32768.0f, 32767.0f);
        const int16_t v = static_cast<int16_t>(std::lrint(x));
        out[2 * i] = static_cast<uint8_t>(v & 0xFF);
        out[2 * i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
    }
}

// ---- Resampler ----------------------------------------------------------------

void Resampler::set_rates(uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == in_rate_ && out_rate == out_rate_) return;
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    step_ = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    reset();
}

void Resampler::reset() {
    buf_.assign(1, 0.0f);  // one sample of (silent) history
    pos_ = 1.0;
}

void Resampler::process(const float* in, size_t n, std::vector<float>& out) {
    if (in_rate_ == out_rate_) {
        out.insert(out.end(), in, in + n);
        return;
    }
    if (buf_.empty()) reset();
    buf_.insert(buf_.end(), in, in + n);
    // Need x[i-1], x[i], x[i+1], x[i+2].
    while (pos_ + 2.0 < static_cast<double>(buf_.size())) {
        const size_t i = static_cast<size_t>(pos_);
        const float t = static_cast<float>(pos_ - static_cast<double>(i));
        const float xm1 = buf_[i - 1], x0 = buf_[i], x1 = buf_[i + 1], x2 = buf_[i + 2];
        const float c0 = x0;
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        out.push_back(((c3 * t + c2) * t + c1) * t + c0);
        pos_ += step_;
    }
    // Drop consumed input, keeping one sample of history before the cursor.
    const size_t keep_from = static_cast<size_t>(pos_) - 1;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep_from));
    pos_ -= static_cast<double>(keep_from);
}

// ---- Concealer ----------------------------------------------------------------

void Concealer::reset() {
    last_.clear();
    gain_ = 1.0f;
    consecutive_ = 0;
}

void Concealer::on_frame(float* frame, size_t n) {
    if (consecutive_ > 0 && n > 0) {
        // Fade in from the concealment level over ~2 ms to avoid a click.
        const size_t ramp = std::min<size_t>(n, 96);
        for (size_t i = 0; i < ramp; ++i) {
            const float g = gain_ + (1.0f - gain_) * static_cast<float>(i) / ramp;
            frame[i] *= g;
        }
    }
    consecutive_ = 0;
    gain_ = 1.0f;
    last_.assign(frame, frame + n);
}

void Concealer::conceal(float* out, size_t n) {
    ++consecutive_;
    const float start = gain_;
    // Halve per lost frame; fully silent after three in a row.
    const float end = consecutive_ >= 3 ? 0.0f : start * 0.5f;
    for (size_t i = 0; i < n; ++i) {
        const float src = last_.empty() ? 0.0f : last_[i % last_.size()];
        const float g = start + (end - start) * static_cast<float>(i) / static_cast<float>(n);
        out[i] = src * g;
    }
    gain_ = end;
}

// ---- GainLimiter --------------------------------------------------------------

void GainLimiter::set_gain_db(float db) {
    gain_ = std::pow(10.0f, std::clamp(db, -40.0f, 24.0f) / 20.0f);
}

void GainLimiter::process(float* s, size_t n) {
    constexpr float kThreshold = 0.97f;
    constexpr float kRelease = 0.9995f;  // ~40 ms at 48 kHz
    for (size_t i = 0; i < n; ++i) {
        const float x = s[i] * gain_;
        const float a = std::fabs(x);
        env_ = std::max(a, env_ * kRelease);
        s[i] = env_ > kThreshold ? x * (kThreshold / env_) : x;
    }
}

float peak_abs(const float* s, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) p = std::max(p, std::fabs(s[i]));
    return p;
}

}  // namespace phonemic
