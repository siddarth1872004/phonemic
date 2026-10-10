#include "phonemic/opus_codec.hpp"

#include <opus.h>

namespace phonemic {

namespace {
constexpr int kMaxFrame = 5760;  // 120 ms at 48 kHz: the largest Opus frame
}

OpusVoiceDecoder::OpusVoiceDecoder() {
    int err = 0;
    dec_ = opus_decoder_create(kRate, 1, &err);
    if (err != OPUS_OK) dec_ = nullptr;
}

OpusVoiceDecoder::~OpusVoiceDecoder() {
    if (dec_) opus_decoder_destroy(dec_);
}

void OpusVoiceDecoder::reset() {
    if (dec_) opus_decoder_ctl(dec_, OPUS_RESET_STATE);
}

bool OpusVoiceDecoder::decode(const uint8_t* data, size_t len, std::vector<float>& out) {
    if (!dec_ || len == 0) return false;
    out.resize(kMaxFrame);
    const int n = opus_decode_float(dec_, data, static_cast<opus_int32>(len), out.data(), kMaxFrame, 0);
    if (n < 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(n));
    return true;
}

bool OpusVoiceDecoder::conceal(const uint8_t* next, size_t next_len, size_t samples, std::vector<float>& out) {
    out.assign(samples, 0.0f);
    if (!dec_ || samples == 0) return false;
    const int frame = static_cast<int>(samples > static_cast<size_t>(kMaxFrame) ? kMaxFrame : samples);
    if (next && next_len > 0) {
        // decode_fec=1: rebuild the *previous* (lost) frame from the LBRR data
        // in the next packet. Falls back to PLC inside libopus if there is none.
        const int n = opus_decode_float(dec_, next, static_cast<opus_int32>(next_len), out.data(), frame, 1);
        if (n > 0) {
            out.resize(static_cast<size_t>(n));
            return opus_packet_has_lbrr(next, static_cast<opus_int32>(next_len)) == 1;
        }
    }
    const int n = opus_decode_float(dec_, nullptr, 0, out.data(), frame, 0);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return false;
}

OpusVoiceEncoder::OpusVoiceEncoder(int bitrate) {
    int err = 0;
    enc_ = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        enc_ = nullptr;
        return;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(1));
}

OpusVoiceEncoder::~OpusVoiceEncoder() {
    if (enc_) opus_encoder_destroy(enc_);
}

int OpusVoiceEncoder::encode(const int16_t* pcm, int samples, uint8_t* out, int max_out) {
    if (!enc_) return -1;
    const int n = opus_encode(enc_, pcm, samples, out, max_out);
    return n < 0 ? -1 : n;
}

}  // namespace phonemic
