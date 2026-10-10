#include "phonemic/denoise.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test.hpp"

using phonemic::Denoiser;

TEST(denoise_one_frame_in_one_frame_out) {
    Denoiser d;
    std::vector<float> in(480), out;
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>((i * 7919) % 4000) / 32768.0f - 0.06f;
    d.process(in.data(), in.size(), out);
    CHECK_EQ(out.size(), 480u);
}

TEST(denoise_buffers_partial_frames) {
    Denoiser d;
    std::vector<float> a(200, 0.0f), b(300, 0.0f), out;
    d.process(a.data(), a.size(), out);
    if (Denoiser::available()) CHECK_EQ(out.size(), 0u);
    d.process(b.data(), b.size(), out);
    CHECK_EQ(out.size(), Denoiser::available() ? 480u : 500u);
}

TEST(denoise_reduces_steady_noise) {
    if (!Denoiser::available()) return;
    Denoiser d;
    uint32_t x = 1;
    std::vector<float> in(48000), out;
    for (auto& s : in) {
        x = x * 1664525u + 1013904223u;
        s = (static_cast<float>(x >> 9) / 8388608.0f - 0.5f) * 0.1f;  // white noise
    }
    d.process(in.data(), in.size(), out);
    double ein = 0, eout = 0;
    for (size_t i = 24000; i < out.size(); ++i) {
        ein += static_cast<double>(in[i]) * in[i];
        eout += static_cast<double>(out[i]) * out[i];
    }
    CHECK(eout < ein * 0.5);
    for (float s : out) CHECK(std::isfinite(s));
}
