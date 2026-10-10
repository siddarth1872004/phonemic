#include "phonemic/voice_fx.hpp"

#include <cmath>
#include <vector>

#include "test.hpp"

using namespace phonemic;

namespace {
std::vector<float> sine(size_t n, float f, float rate = 48000.0f) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = 0.37f * std::sin(6.2831853f * f * static_cast<float>(i) / rate);
    return v;
}
size_t zero_crossings(const float* s, size_t n) {
    size_t c = 0;
    for (size_t i = 1; i < n; ++i) c += (s[i - 1] < 0) != (s[i] < 0);
    return c;
}
float pitch_ratio(Effect e) {
    VoiceFx fx;
    fx.set_effect(e);
    auto buf = sine(48000, 440.0f);
    const auto orig = buf;
    fx.process(buf.data(), buf.size());
    const size_t skip = VoiceFx::kWindow * 2;  // fill transient
    return static_cast<float>(zero_crossings(buf.data() + skip, buf.size() - skip)) /
           static_cast<float>(zero_crossings(orig.data() + skip, orig.size() - skip));
}
double energy(const std::vector<float>& v) {
    double e = 0;
    for (float x : v) e += static_cast<double>(x) * x;
    return e;
}
}  // namespace

TEST(fx_none_is_passthrough) {
    VoiceFx fx;
    auto buf = sine(4800, 440.0f);
    const auto orig = buf;
    fx.process(buf.data(), buf.size());
    CHECK(buf == orig);
}

TEST(fx_deep_lowers_pitch) {
    const float r = pitch_ratio(Effect::Deep);
    CHECK(r > 0.65f && r < 0.85f);
}

TEST(fx_chipmunk_raises_pitch) {
    const float r = pitch_ratio(Effect::Chipmunk);
    CHECK(r > 1.25f && r < 1.6f);
}

TEST(fx_robot_produces_nonsilent_bounded_output) {
    VoiceFx fx;
    fx.set_effect(Effect::Robot);
    auto buf = sine(9600, 200.0f);
    fx.process(buf.data(), buf.size());
    CHECK(energy(buf) > 0.0);
    for (float x : buf) CHECK(std::fabs(x) <= 1.0f);
}

TEST(fx_radio_cuts_lows_keeps_mids) {
    auto run = [](float f) {
        VoiceFx fx;
        fx.set_effect(Effect::Radio);
        auto b = sine(48000, f);
        fx.process(b.data(), b.size());
        return energy(std::vector<float>(b.begin() + 4800, b.end()));
    };
    CHECK(run(1000.0f) > 8.0 * run(80.0f));  // 80 Hz rumble removed vs 1 kHz voice band
}

TEST(fx_echo_adds_delayed_copy) {
    VoiceFx fx;
    fx.set_effect(Effect::Echo);
    std::vector<float> b(48000, 0.0f);
    b[0] = 1.0f;  // impulse
    fx.process(b.data(), b.size());
    const size_t d = static_cast<size_t>(48000 * 0.22f);
    CHECK_NEAR(b[d], 0.45, 1e-4);
    CHECK_NEAR(b[d * 2], 0.45 * 0.35, 1e-4);
}

TEST(fx_switching_is_safe_and_finite) {
    VoiceFx fx;
    auto buf = sine(4800, 300.0f);
    for (int e = 0; e < kEffectCount; ++e) {
        fx.set_effect(static_cast<Effect>(e));
        fx.process(buf.data(), buf.size());
        for (float x : buf) CHECK(std::isfinite(x));
    }
    CHECK(effect_from_u8(200) == Effect::None);
}
