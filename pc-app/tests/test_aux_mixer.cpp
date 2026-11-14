// The phone's soundboard mixer (phone-app/native/aux_mixer.h): header-only
// and Android-free, so it is tested here with the rest of the suite.
#include "aux_mixer.h"

#include <cmath>
#include <cstdlib>
#include <vector>

#include "test.hpp"

using namespace phonemic;

namespace {
constexpr size_t kFrame = 480;  // 10 ms @ 48 kHz, what the sender mixes per call

std::vector<int16_t> tone(size_t n, float amp, float hz, float rate) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<int16_t>(std::lround(amp * 32767.0f * std::sin(6.2831853f * hz * static_cast<float>(i) / rate)));
    return v;
}

void feed(AuxRing& ring, AuxResampler& rs, const std::vector<int16_t>& pcm, int channels) {
    const size_t frames = pcm.size() / static_cast<size_t>(channels);
    CHECK_EQ(rs.process(pcm.data(), frames, ring), frames);
}

size_t drain(AuxRing& ring) {
    std::vector<int16_t> tmp(kFrame);
    size_t total = 0;
    for (size_t got; (got = ring.pop(tmp.data(), tmp.size())) > 0;) total += got;
    return total;
}
}  // namespace

TEST(aux_mic_only_is_bit_exact) {
    AuxRing ring;
    AuxMixer mix;
    const auto mic = tone(kFrame, 0.95f, 440, 48000);
    auto out = mic;
    mix.mix(out.data(), out.size(), ring, false);
    CHECK(out == mic);  // no aux queued: not even the limiter touches it
}

TEST(aux_only_reaches_output_at_gain) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    mix.set_gain(1.0f);
    rs.begin(48000, 1, 48000);
    const auto clip = tone(kFrame * 3, 0.5f, 1000, 48000);
    feed(ring, rs, clip, 1);
    std::vector<int16_t> out(kFrame, 0);
    mix.mix(out.data(), out.size(), ring, false);
    for (size_t i = 0; i < kFrame; ++i) CHECK_EQ(out[i], clip[i]);
}

TEST(aux_mixes_with_mic) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    mix.set_gain(0.5f);
    rs.begin(48000, 1, 48000);
    std::vector<int16_t> clip(kFrame * 2, 4000);
    feed(ring, rs, clip, 1);
    std::vector<int16_t> out(kFrame, 1000);
    mix.mix(out.data(), out.size(), ring, false);
    for (size_t i = 0; i < kFrame; ++i) CHECK_EQ(out[i], 3000);  // 1000 + 4000 * 0.5
}

TEST(aux_loud_sum_never_clips_or_wraps) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    mix.set_gain(2.0f);
    rs.begin(48000, 1, 48000);
    feed(ring, rs, tone(kFrame * 20, 1.0f, 300, 48000), 1);
    const auto mic = tone(kFrame * 20, 1.0f, 300, 48000);  // in phase: the worst case
    int peak = 0;
    for (size_t f = 0; f < 20; ++f) {
        std::vector<int16_t> out(mic.begin() + static_cast<long>(f * kFrame), mic.begin() + static_cast<long>((f + 1) * kFrame));
        mix.mix(out.data(), out.size(), ring, false);
        for (size_t i = 0; i < kFrame; ++i) {
            peak = std::max(peak, std::abs(static_cast<int>(out[i])));
            // A wrapped sum would flip sign against the (in-phase) inputs.
            if (std::abs(mic[f * kFrame + i]) > 2000) CHECK((out[i] > 0) == (mic[f * kFrame + i] > 0));
        }
    }
    CHECK(peak <= static_cast<int>(0.9f * 32767.0f) + 1);
    CHECK(peak > 20000);  // limited, not silenced
}

TEST(aux_resamples_44k1_stereo_to_48k_mono) {
    AuxRing ring;
    AuxResampler rs;
    rs.begin(44100, 2, 48000);
    const size_t frames = 44100;  // one second
    const auto mono = tone(frames, 0.5f, 1000, 44100);
    std::vector<int16_t> stereo(frames * 2);
    for (size_t i = 0; i < frames; ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];

    std::vector<int16_t> out, tmp(kFrame);
    for (size_t off = 0; off < frames;) {
        off += rs.process(stereo.data() + off * 2, frames - off, ring);
        for (size_t got; (got = ring.pop(tmp.data(), tmp.size())) > 0;)
            out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<long>(got));
    }
    // ~48000 samples for one second of input, and still a 1 kHz tone.
    CHECK(out.size() >= 47990 && out.size() <= 48001);
    size_t crossings = 0;
    for (size_t i = 1; i < 48000 && i < out.size(); ++i) crossings += (out[i - 1] < 0) != (out[i] < 0);
    CHECK_NEAR(static_cast<double>(crossings), 2000.0, 4.0);
}

TEST(aux_short_clip_then_silence_returns_to_bit_exact) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(100, 3000), 1);
    const auto mic = tone(kFrame, 0.3f, 440, 48000);
    auto out = mic;
    mix.mix(out.data(), out.size(), ring, false);  // partial frame of aux, rest is mic only
    CHECK(out != mic);
    out = mic;
    mix.mix(out.data(), out.size(), ring, false);  // queue empty, limiter idle
    CHECK(out == mic);
}

TEST(aux_queue_is_bounded_and_producer_backs_off) {
    AuxRing ring;
    AuxResampler rs;
    rs.begin(48000, 1, 48000);
    const std::vector<int16_t> big(AuxRing::kSize * 2, 100);
    const size_t took = rs.process(big.data(), big.size(), ring);
    CHECK(took < big.size());
    CHECK(took <= AuxRing::kSize + 1);
    CHECK(ring.room() < 2);
    CHECK_EQ(rs.process(big.data(), big.size(), ring), size_t{0});  // full: nothing taken, nothing lost
    CHECK(drain(ring) <= AuxRing::kSize);
}

TEST(aux_discard_drops_previous_clip) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    mix.set_gain(1.0f);
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(kFrame * 4, 5000), 1);
    ring.discard();  // a new press
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(kFrame * 2, -2000), 1);
    std::vector<int16_t> out(kFrame, 0);
    mix.mix(out.data(), out.size(), ring, false);
    for (size_t i = 0; i < kFrame; ++i) CHECK(out[i] <= 0);  // none of the old clip
}

TEST(aux_stop_and_restart_leave_no_stale_audio) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(kFrame * 4, 5000), 1);
    ring.drop_all();  // what AudioEngine::start() does before the sender runs
    mix.reset();
    const auto mic = tone(kFrame, 0.3f, 440, 48000);
    auto out = mic;
    mix.mix(out.data(), out.size(), ring, false);
    CHECK(out == mic);
}

TEST(aux_muted_consumes_without_mixing) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(kFrame * 2, 5000), 1);
    std::vector<int16_t> out(kFrame, 0);
    mix.mix(out.data(), out.size(), ring, true);
    for (int16_t s : out) CHECK_EQ(s, 0);
    mix.mix(out.data(), out.size(), ring, true);
    CHECK_EQ(drain(ring), size_t{0});  // nothing left to play late after unmuting
}

TEST(aux_latency_is_one_frame_after_queueing) {
    AuxRing ring;
    AuxResampler rs;
    AuxMixer mix;
    mix.set_gain(1.0f);
    rs.begin(48000, 1, 48000);
    feed(ring, rs, std::vector<int16_t>(kFrame, 8000), 1);
    std::vector<int16_t> out(kFrame, 0);
    mix.mix(out.data(), out.size(), ring, false);
    CHECK_EQ(out[10], 8000);  // in the very next frame the sender cuts
}

TEST(aux_rejects_bad_formats) {
    CHECK(!AuxResampler::valid(0, 1));
    CHECK(!AuxResampler::valid(48000, 0));
    CHECK(!AuxResampler::valid(48000, 9));
    CHECK(!AuxResampler::valid(1000000, 2));
    CHECK(AuxResampler::valid(44100, 2));
}
