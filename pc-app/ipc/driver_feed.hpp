// Windows only: an AudioSinkIface that writes into the shared section the
// (experimental) PhoneMic ACX driver reads — the replacement for VB-CABLE.
// Creating a Global\ section needs administrator rights, as does loading the
// driver. Used by `phonemic-receiver --driver`.
#pragma once

#ifdef _WIN32

#include <memory>
#include <string>

#include "phonemic/receiver.hpp"

namespace phonemic {

class DriverFeed final : public AudioSinkIface {
public:
    static std::unique_ptr<DriverFeed> create(std::string* err);
    ~DriverFeed() override;
    void write(const float* samples, size_t n) override;

private:
    DriverFeed() = default;
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    struct State;
    std::unique_ptr<State> st_;
};

}  // namespace phonemic

#endif
