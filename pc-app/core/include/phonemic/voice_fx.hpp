// Real-time voice effects on 48 kHz mono float audio.
//
// The pitch shifter is the classic dual-tap crossfading delay line: two read
// taps sweep a short ring at a rate offset from the write rate (replaying the
// audio faster/slower = higher/lower pitch) and a triangular crossfade hides
// each tap's jump. Cheap, dependency-free, ~one window (43 ms) of latency —
// a fun voice changer, not a studio formant-preserving shifter.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phonemic {

enum class Effect : uint8_t {
    None = 0,
    Deep = 1,      // pitch down ~5 semitones
    Chipmunk = 2,  // pitch up ~6 semitones
    Robot = 3,     // ring modulation
    Radio = 4,     // telephone/walkie-talkie band-pass + grit
    Echo = 5,      // feedback delay ("cave")
};
constexpr int kEffectCount = 6;

Effect effect_from_u8(uint8_t v);
const char* effect_name(Effect e);

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    static Biquad highpass(float fc, float q, float fs);
    static Biquad lowpass(float fc, float q, float fs);
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

class VoiceFx {
public:
    static constexpr size_t kWindow = 2048;  // ~43 ms @ 48 kHz
    static constexpr size_t kRing = kWindow * 2;
    static constexpr float kSampleRate = 48000.0f;

    VoiceFx();
    void set_effect(Effect e);
    Effect effect() const { return effect_; }
    // Process in place. Samples are nominally in [-1, 1].
    void process(float* samples, size_t n);

private:
    float tap(float delay) const;
    void reset_state();

    std::array<float, kRing> ring_{};
    size_t write_ = 0;
    float phase_ = 0.0f;
    float osc_ = 0.0f;
    Effect effect_ = Effect::None;
    Biquad hp_, lp_;
    std::vector<float> echo_;
    size_t echo_pos_ = 0;
};

}  // namespace phonemic
