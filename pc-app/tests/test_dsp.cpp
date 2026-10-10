#include "phonemic/dsp.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

#include "test.hpp"

using namespace phonemic;

namespace {
std::vector<float> sine(size_t n, float f, float rate) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = 0.5f * std::sin(6.2831853f * f * static_cast<float>(i) / rate);
    return v;
}
size_t zero_crossings(const std::vector<float>& s) {
    size_t c = 0;
    for (size_t i = 1; i < s.size(); ++i) c += (s[i - 1] < 0) != (s[i] < 0);
    return c;
}
}  // namespace

TEST(dsp_pcm16_roundtrip_and_extremes) {
    const uint8_t b[] = {0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00};  // -32768, 32767, 0
    float f[3];
    pcm16le_to_float(b, 3, f);
    CHECK_NEAR(f[0], -1.0, 1e-6);
    CHECK_NEAR(f[1], 32767.0 / 32768.0, 1e-6);
    CHECK_NEAR(f[2], 0.0, 1e-9);
    uint8_t out[6];
    float g[3] = {-2.0f, 2.0f, 0.0f};  // out of range clamps instead of wrapping
    float_to_pcm16le(g, 3, out);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x80);
    CHECK_EQ(out[2], 0xFF);
    CHECK_EQ(out[3], 0x7F);
}

TEST(dsp_resampler_44k1_to_48k_preserves_pitch_and_length) {
    Resampler r;
    r.set_rates(44100, 48000);
    const auto in = sine(44100, 1000.0f, 44100.0f);
    std::vector<float> out;
    for (size_t i = 0; i < in.size(); i += 441) r.process(in.data() + i, 441, out);  // 10 ms blocks
    CHECK(std::abs(static_cast<long>(out.size()) - 48000) < 10);
    // Same number of cycles per second => same frequency.
    CHECK(std::abs(static_cast<long>(zero_crossings(out)) - 2000) <= 4);
}

TEST(dsp_resampler_identity_is_exact) {
    Resampler r;
    r.set_rates(48000, 48000);
    const auto in = sine(480, 440.0f, 48000.0f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);
    CHECK(out == in);
}

TEST(dsp_resampler_is_smooth_across_blocks) {
    Resampler r;
    r.set_rates(16000, 48000);
    const auto in = sine(16000, 200.0f, 16000.0f);
    std::vector<float> out;
    for (size_t i = 0; i < in.size(); i += 160) r.process(in.data() + i, 160, out);
    float max_step = 0;
    for (size_t i = 1; i < out.size(); ++i) max_step = std::max(max_step, std::fabs(out[i] - out[i - 1]));
    // A 200 Hz, 0.5-amplitude sine at 48 kHz moves at most ~0.0131 per sample.
    CHECK(max_step < 0.02f);
}

TEST(dsp_concealer_fades_out_then_back_in) {
    Concealer c;
    std::vector<float> f(480, 0.5f);
    c.on_frame(f.data(), f.size());
    std::vector<float> o(480);
    c.conceal(o.data(), o.size());
    CHECK_NEAR(o[0], 0.5, 1e-6);         // continues from the last frame
    CHECK(o[479] < 0.26f);                // fading
    c.conceal(o.data(), o.size());
    c.conceal(o.data(), o.size());
    CHECK_NEAR(o[479], 0.0, 1e-3);        // silent after three losses
    std::vector<float> g(480, 0.5f);
    c.on_frame(g.data(), g.size());
    CHECK(g[0] < 0.1f);                   // ramps back in, no click
    CHECK_NEAR(g[479], 0.5, 1e-6);
}

TEST(dsp_limiter_never_clips_even_with_boost) {
    GainLimiter g;
    g.set_gain_db(18.0f);
    auto s = sine(4800, 300.0f, 48000.0f);
    g.process(s.data(), s.size());
    float peak = peak_abs(s.data(), s.size());
    CHECK(peak <= 0.971f);
    CHECK(peak > 0.9f);
}

TEST(dsp_gain_zero_db_is_transparent_for_quiet_audio) {
    GainLimiter g;
    g.set_gain_db(0.0f);
    auto s = sine(480, 300.0f, 48000.0f);
    const auto orig = s;
    g.process(s.data(), s.size());
    for (size_t i = 0; i < s.size(); ++i) CHECK_NEAR(s[i], orig[i], 1e-6);
}
