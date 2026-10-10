#include "phonemic/denoise.hpp"

#include <algorithm>
#include <cmath>

#ifdef PHONEMIC_HAVE_RNNOISE
#include <rnnoise.h>
#endif

namespace phonemic {

bool Denoiser::available() {
#ifdef PHONEMIC_HAVE_RNNOISE
    return true;
#else
    return false;
#endif
}

Denoiser::Denoiser() {
#ifdef PHONEMIC_HAVE_RNNOISE
    st_ = rnnoise_create(nullptr);  // nullptr = the built-in model
#endif
    inbuf_.reserve(kFrame * 4);
}

Denoiser::~Denoiser() {
#ifdef PHONEMIC_HAVE_RNNOISE
    if (st_) rnnoise_destroy(st_);
#endif
}

void Denoiser::reset() {
    inbuf_.clear();
#ifdef PHONEMIC_HAVE_RNNOISE
    if (st_) rnnoise_destroy(st_);
    st_ = rnnoise_create(nullptr);
#endif
}

void Denoiser::process(const float* in, size_t n, std::vector<float>& out) {
#ifdef PHONEMIC_HAVE_RNNOISE
    // RNNoise expects samples at int16 magnitude, not -1..1.
    for (size_t i = 0; i < n; ++i) inbuf_.push_back(in[i] * 32768.0f);
    float frame_out[kFrame];
    size_t consumed = 0;
    while (inbuf_.size() - consumed >= kFrame) {
        vad_ = rnnoise_process_frame(st_, frame_out, inbuf_.data() + consumed);
        for (size_t i = 0; i < kFrame; ++i)
            out.push_back(std::clamp(frame_out[i] / 32768.0f, -1.0f, 1.0f));
        consumed += kFrame;
    }
    inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
#else
    out.insert(out.end(), in, in + n);
#endif
}

}  // namespace phonemic
