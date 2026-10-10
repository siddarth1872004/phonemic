#include "driver_feed.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ring_producer.hpp"

namespace phonemic {

struct DriverFeed::State {
    std::unique_ptr<RingProducer> producer;
    std::vector<int16_t> pcm;
};

std::unique_ptr<DriverFeed> DriverFeed::create(std::string* err) {
    // Must match PHONEMIC_RING_SECTION_NAME in the driver (\BaseNamedObjects\...
    // is the Win32 Global\ namespace).
    const DWORD size = static_cast<DWORD>(sizeof(PHONEMIC_RING));
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size,
                                        L"Global\\PhonemicRing");
    if (!mapping) {
        if (err) *err = "CreateFileMapping(Global\\PhonemicRing) failed (" + std::to_string(GetLastError()) +
                        ") — run as administrator";
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        if (err) *err = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return nullptr;
    }
    auto* ring = static_cast<PHONEMIC_RING*>(view);
    ring_init_header(ring);
    std::unique_ptr<DriverFeed> f(new DriverFeed());
    f->mapping_ = mapping;
    f->view_ = view;
    f->st_ = std::make_unique<State>();
    f->st_->producer = std::make_unique<RingProducer>(ring);
    return f;
}

DriverFeed::~DriverFeed() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
}

void DriverFeed::write(const float* s, size_t n) {
    st_->pcm.resize(n);
    for (size_t i = 0; i < n; ++i)
        st_->pcm[i] = static_cast<int16_t>(std::lrint(std::clamp(s[i], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
    st_->producer->push(st_->pcm.data(), n);
}

}  // namespace phonemic
