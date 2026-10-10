#include "phonemic/voice_fx.hpp"

#include <algorithm>
#include <cmath>

namespace phonemic {

namespace {
constexpr float kTau = 6.28318530717958647692f;
}  // namespace

Effect effect_from_u8(uint8_t v) {
    return v < kEffectCount ? static_cast<Effect>(v) : Effect::None;
}

const char* effect_name(Effect e) {
    switch (e) {
        case Effect::None: return "Normal";
        case Effect::Deep: return "Deep";
        case Effect::Chipmunk: return "Chipmunk";
        case Effect::Robot: return "Robot";
        case Effect::Radio: return "Radio";
        case Effect::Echo: return "Echo";
    }
    return "Normal";
}

// RBJ audio-EQ-cookbook biquads.
Biquad Biquad::highpass(float fc, float q, float fs) {
    const float w = kTau * fc / fs, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
    const float a0 = 1 + alpha;
    Biquad b;
    b.b0 = (1 + cw) / 2 / a0;
    b.b1 = -(1 + cw) / a0;
    b.b2 = (1 + cw) / 2 / a0;
    b.a1 = -2 * cw / a0;
    b.a2 = (1 - alpha) / a0;
    return b;
}

Biquad Biquad::lowpass(float fc, float q, float fs) {
    const float w = kTau * fc / fs, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
    const float a0 = 1 + alpha;
    Biquad b;
    b.b0 = (1 - cw) / 2 / a0;
    b.b1 = (1 - cw) / a0;
    b.b2 = (1 - cw) / 2 / a0;
    b.a1 = -2 * cw / a0;
    b.a2 = (1 - alpha) / a0;
    return b;
}

VoiceFx::VoiceFx() { reset_state(); }

void VoiceFx::reset_state() {
    phase_ = 0.0f;
    osc_ = 0.0f;
    hp_ = Biquad::highpass(400.0f, 0.707f, kSampleRate);
    lp_ = Biquad::lowpass(3000.0f, 0.707f, kSampleRate);
    echo_.assign(static_cast<size_t>(kSampleRate * 0.22f), 0.0f);
    echo_pos_ = 0;
}

void VoiceFx::set_effect(Effect e) {
    if (e != effect_) {
        effect_ = e;
        reset_state();
    }
}

float VoiceFx::tap(float delay) const {
    float pos = static_cast<float>(write_) - 1.0f - delay;
    pos = std::fmod(pos, static_cast<float>(kRing));
    if (pos < 0) pos += static_cast<float>(kRing);
    const size_t i0 = static_cast<size_t>(pos) % kRing;
    const size_t i1 = (i0 + 1) % kRing;
    const float frac = pos - std::floor(pos);
    return ring_[i0] * (1.0f - frac) + ring_[i1] * frac;
}

void VoiceFx::process(float* s, size_t n) {
    switch (effect_) {
        case Effect::None:
            return;
        case Effect::Robot: {
            const float f = 60.0f / kSampleRate;  // 60 Hz ring-mod: metallic monotone
            for (size_t i = 0; i < n; ++i) {
                s[i] *= std::sin(kTau * osc_);
                osc_ += f;
                if (osc_ >= 1.0f) osc_ -= 1.0f;
            }
            return;
        }
        case Effect::Radio:
            for (size_t i = 0; i < n; ++i) {
                float x = lp_.process(hp_.process(s[i]));
                x = std::tanh(x * 3.0f) * 0.6f;  // soft-clipped "small speaker" grit
                s[i] = x;
            }
            return;
        case Effect::Echo:
            for (size_t i = 0; i < n; ++i) {
                const float delayed = echo_[echo_pos_];
                const float out = s[i] + 0.45f * delayed;
                echo_[echo_pos_] = s[i] + 0.35f * delayed;  // feedback
                echo_pos_ = (echo_pos_ + 1) % echo_.size();
                s[i] = out;
            }
            return;
        case Effect::Deep:
        case Effect::Chipmunk: {
            const float ratio = effect_ == Effect::Deep ? 0.75f : 1.41f;
            // The delay sweeps by (1 - ratio) per sample: ratio < 1 falls behind
            // (slower replay, lower pitch); ratio > 1 catches up (higher pitch).
            const float step = 1.0f - ratio;
            const float w = static_cast<float>(kWindow);
            for (size_t i = 0; i < n; ++i) {
                ring_[write_] = s[i];
                write_ = (write_ + 1) % kRing;
                phase_ = std::fmod(phase_ + step, w);
                if (phase_ < 0) phase_ += w;
                const float d1 = phase_;
                const float d2 = std::fmod(phase_ + w / 2.0f, w);
                const float x = d1 / w;
                const float g1 = 1.0f - std::fabs(2.0f * x - 1.0f);  // 0 when tap 1 jumps
                const float g2 = 1.0f - g1;
                s[i] = tap(d1) * g1 + tap(d2) * g2;
            }
            return;
        }
    }
}

}  // namespace phonemic
