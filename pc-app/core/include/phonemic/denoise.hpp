// Voice Focus: RNNoise noise suppression on 48 kHz mono float audio.
//
// RNNoise works on fixed 480-sample (10 ms) frames — our packet size — so in the
// normal case each packet is exactly one frame. Input is buffered anyway so any
// packet size works; the extra latency is bounded by one frame. When built
// without RNNoise (PHONEMIC_WITH_RNNOISE=OFF) this is a pass-through and
// available() is false so the UI can hide the toggle.
#pragma once

#include <cstddef>
#include <vector>

struct DenoiseState;

namespace phonemic {

class Denoiser {
public:
    static constexpr size_t kFrame = 480;
    static bool available();

    Denoiser();
    ~Denoiser();
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // Denoise `n` samples, appending results to `out`. Output may lag input by up
    // to one frame while the buffer fills.
    void process(const float* in, size_t n, std::vector<float>& out);
    // Voice-activity probability of the most recent frame (0..1).
    float vad() const { return vad_; }
    void reset();

private:
    DenoiseState* st_ = nullptr;
    std::vector<float> inbuf_;
    float vad_ = 0.0f;
};

}  // namespace phonemic
