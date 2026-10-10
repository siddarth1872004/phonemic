// Small DSP building blocks used by the receive pipeline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace phonemic {

// PCM16 little-endian bytes <-> float [-1, 1).
void pcm16le_to_float(const uint8_t* bytes, size_t samples, float* out);
void float_to_pcm16le(const float* in, size_t samples, uint8_t* out);

// Streaming sample-rate converter (4-point cubic Hermite). Converts whatever
// rate a sender uses (e.g. a browser stuck at 44.1 kHz) to the 48 kHz pipeline
// rate, keeping state across calls so block boundaries are seamless.
class Resampler {
public:
    void set_rates(uint32_t in_rate, uint32_t out_rate);
    uint32_t in_rate() const { return in_rate_; }
    // Appends converted samples to `out`.
    void process(const float* in, size_t n, std::vector<float>& out);
    void reset();

private:
    uint32_t in_rate_ = 48000, out_rate_ = 48000;
    double step_ = 1.0;  // input samples advanced per output sample
    double pos_ = 1.0;   // read position in buf_ (needs 1 sample of history)
    std::vector<float> buf_;
};

// Packet-loss concealment for PCM: replay the last good frame with a fast fade
// (instead of a hard cut to silence, which clicks), then fade back in when real
// audio resumes.
class Concealer {
public:
    // A real frame arrived: remember it and smooth the transition if we were
    // concealing.
    void on_frame(float* frame, size_t n);
    // A frame was lost: synthesize `n` samples into out.
    void conceal(float* out, size_t n);
    void reset();

private:
    std::vector<float> last_;
    float gain_ = 1.0f;
    int consecutive_ = 0;
};

// Gain followed by a peak limiter (instant attack, smooth release) so a gain
// boost can never clip.
class GainLimiter {
public:
    void set_gain_db(float db);
    void process(float* s, size_t n);

private:
    float gain_ = 1.0f;
    float env_ = 0.0f;
};

float peak_abs(const float* s, size_t n);

}  // namespace phonemic
