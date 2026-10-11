// Windows only: the audio sink that feeds the PhoneMic ACX driver (../driver),
// PhoneMic's own virtual microphone. The driver creates the shared ring
// Global\PhonemicRing when its device starts; this opens it (no administrator
// rights needed) and keeps it fed through a RingFeeder.
#pragma once

#ifdef _WIN32

#include <memory>
#include <string>

#include "phonemic/playout.hpp"
#include "phonemic/receiver.hpp"

namespace phonemic {

class RingFeeder;

class DriverFeed final : public AudioSinkIface {
public:
    // nullptr (with err) if the driver isn't installed / running.
    static std::unique_ptr<DriverFeed> open(std::string* err);
    // Is the driver's ring there right now? (Cheap: open + close.)
    static bool available();
    ~DriverFeed() override;

    void write(const float* samples, size_t n) override;
    void flush() override;
    uint32_t buffered_ms() const override;
    void set_latency_ms(uint32_t ms);
    AdaptivePlayout::Stats stats() const;
    // Samples the driver has read so far: grows only while some app records.
    uint64_t consumed() const;

private:
    DriverFeed() = default;
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    std::unique_ptr<RingFeeder> feeder_;
};

}  // namespace phonemic

#endif
