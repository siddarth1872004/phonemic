// Run an external program (no shell), capture its output, with a timeout.
// Windows: no console window flashes up (CREATE_NO_WINDOW).
#pragma once

#include <string>
#include <vector>

namespace phonemic {

struct ProcessResult {
    bool started = false;  // false: the program could not be launched at all
    int exit_code = -1;    // -1 if it was killed for exceeding the timeout
    std::string output;    // stdout + stderr
};

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_ms);

}  // namespace phonemic
