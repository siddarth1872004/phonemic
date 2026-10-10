#include <cstring>
#include <exception>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (const auto& c : t::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        try {
            c.fn();
            ++passed;
            std::printf("  ok    %s\n", c.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("  FAIL  %s\n        %s\n", c.name, e.what());
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
