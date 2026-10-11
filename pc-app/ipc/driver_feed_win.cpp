#include "driver_feed.hpp"

#include <windows.h>

#include "ring_feeder.hpp"

namespace phonemic {

namespace {
// \BaseNamedObjects\PhonemicRing in the driver (PHONEMIC_RING_SECTION_NAME).
constexpr wchar_t kRingName[] = L"Global\\PhonemicRing";
}  // namespace

std::unique_ptr<DriverFeed> DriverFeed::open(std::string* err) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kRingName);
    if (!mapping) {
        const DWORD e = GetLastError();
        if (err)
            *err = e == ERROR_FILE_NOT_FOUND ? "the PhoneMic driver is not installed or not running"
                                             : "could not open the PhoneMic driver's ring (" + std::to_string(e) + ")";
        return nullptr;
    }
    // Fails if the section is smaller than the ring.
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PHONEMIC_RING));
    if (!view) {
        if (err) *err = "could not map the PhoneMic driver's ring (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return nullptr;
    }
    auto* ring = static_cast<PHONEMIC_RING*>(view);
    if (!ring_header_ok(ring)) {
        if (err) *err = "the PhoneMic driver and app versions don't match (reinstall the driver)";
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return nullptr;
    }
    std::unique_ptr<DriverFeed> f(new DriverFeed());
    f->mapping_ = mapping;
    f->view_ = view;
    f->feeder_ = std::make_unique<RingFeeder>(ring);
    f->feeder_->start();
    return f;
}

bool DriverFeed::available() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kRingName);
    if (!mapping) return false;
    bool ok = false;
    if (void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(PHONEMIC_RING))) {
        ok = ring_header_ok(static_cast<const PHONEMIC_RING*>(view));
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return ok;
}

DriverFeed::~DriverFeed() {
    feeder_.reset();  // stops the feeder thread before the view goes away
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
}

void DriverFeed::write(const float* s, size_t n) { feeder_->write(s, n); }
void DriverFeed::flush() { feeder_->flush(); }
uint32_t DriverFeed::buffered_ms() const { return feeder_->buffered_ms(); }
void DriverFeed::set_latency_ms(uint32_t ms) { feeder_->set_latency_ms(ms); }
AdaptivePlayout::Stats DriverFeed::stats() const { return feeder_->stats(); }
uint64_t DriverFeed::consumed() const { return feeder_->consumed(); }

}  // namespace phonemic
