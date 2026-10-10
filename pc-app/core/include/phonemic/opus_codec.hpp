// Thin RAII wrappers over libopus for the 48 kHz mono voice stream.
//
// Opus (codec 1 on the wire) cuts the stream from 768 kbps PCM to ~48 kbps,
// and brings real packet-loss concealment plus in-band forward error
// correction: a lost frame can often be rebuilt from redundancy carried in the
// *next* packet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace phonemic {

class OpusVoiceDecoder {
public:
    static constexpr int kRate = 48000;
    OpusVoiceDecoder();
    ~OpusVoiceDecoder();
    OpusVoiceDecoder(const OpusVoiceDecoder&) = delete;
    OpusVoiceDecoder& operator=(const OpusVoiceDecoder&) = delete;

    void reset();
    // Decode one packet into `out` (resized). Returns false on a corrupt packet.
    bool decode(const uint8_t* data, size_t len, std::vector<float>& out);
    // Synthesize a lost frame of `samples` samples: from `next_packet`'s FEC
    // data when given (nullptr = plain concealment). Returns true if FEC data
    // was actually used.
    bool conceal(const uint8_t* next_packet, size_t next_len, size_t samples, std::vector<float>& out);

private:
    OpusDecoder* dec_ = nullptr;
};

class OpusVoiceEncoder {
public:
    // `bitrate` in bits/s. Expects 10 ms frames (480 samples) at 48 kHz.
    explicit OpusVoiceEncoder(int bitrate = 48000);
    ~OpusVoiceEncoder();
    OpusVoiceEncoder(const OpusVoiceEncoder&) = delete;
    OpusVoiceEncoder& operator=(const OpusVoiceEncoder&) = delete;

    bool ok() const { return enc_ != nullptr; }
    // Returns the packet length, or -1 on error.
    int encode(const int16_t* pcm, int samples, uint8_t* out, int max_out);

private:
    OpusEncoder* enc_ = nullptr;
};

}  // namespace phonemic
