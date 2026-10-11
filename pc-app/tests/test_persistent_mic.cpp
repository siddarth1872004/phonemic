// Linux: keeping "PhoneMic Microphone" registered when PhoneMic is closed. The
// config edits must be exact: a broken default.pa means no sound at all.
#if defined(__linux__)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "phonemic/virtual_mic.hpp"
#include "test.hpp"

namespace fs = std::filesystem;

namespace {
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/phonemic-cfg-XXXXXX";
        path = ::mkdtemp(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
std::string slurp(const std::string& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void spit(const std::string& p, const std::string& text) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p) << text;
}
size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (auto at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}
}  // namespace

TEST(persistent_mic_pipewire_dropin) {
    TempDir cfg;
    std::string err;
    CHECK(!phonemic::persistent_mic_installed(cfg.path));
    CHECK(phonemic::set_persistent_mic(true, /*pipewire=*/true, &err, cfg.path));
    const std::string f = cfg.path + "/pipewire/pipewire-pulse.conf.d/90-phonemic.conf";
    const std::string text = slurp(f);
    CHECK(text.find("pulse.cmd = [") != std::string::npos);
    CHECK(text.find("module-null-sink sink_name=PhoneMic") != std::string::npos);
    // The nested quotes are escaped for SPA JSON (checked against real PipeWire).
    CHECK(text.find("source_properties=\\\"device.description='PhoneMic Microphone'") != std::string::npos);
    CHECK_EQ(count(text, "\"nofail\""), 2u);
    CHECK(phonemic::persistent_mic_installed(cfg.path));
    CHECK(phonemic::set_persistent_mic(false, true, &err, cfg.path));
    CHECK(!fs::exists(f));
    CHECK(!phonemic::persistent_mic_installed(cfg.path));
}

TEST(persistent_mic_pulseaudio_new_file_includes_the_system_one) {
    TempDir cfg, sys;
    const std::string system_pa = sys.path + "/default.pa";
    spit(system_pa, "load-module module-udev-detect\n");
    std::string err;
    CHECK(phonemic::set_persistent_mic(true, false, &err, cfg.path, system_pa));
    const std::string f = cfg.path + "/pulse/default.pa";
    const std::string text = slurp(f);
    CHECK(text.find(".include " + system_pa + "\n") != std::string::npos);
    CHECK(text.find("load-module module-remap-source master=PhoneMic.monitor") != std::string::npos);
    // Our modules may never stop PulseAudio from starting.
    CHECK(text.find(".nofail\nload-module module-null-sink") != std::string::npos);
    CHECK(text.find(".fail\n### PhoneMic end") != std::string::npos);
    // Idempotent.
    CHECK(phonemic::set_persistent_mic(true, false, &err, cfg.path, system_pa));
    CHECK_EQ(count(slurp(f), "### PhoneMic begin"), 1u);
    // Undo removes the file PhoneMic created.
    CHECK(phonemic::set_persistent_mic(false, false, &err, cfg.path, system_pa));
    CHECK(!fs::exists(f));
}

TEST(persistent_mic_pulseaudio_keeps_the_users_own_file_intact) {
    TempDir cfg;
    const std::string f = cfg.path + "/pulse/default.pa";
    const std::string mine = ".include /etc/pulse/default.pa\nload-module module-echo-cancel\n";
    spit(f, mine);
    std::string err;
    CHECK(phonemic::set_persistent_mic(true, false, &err, cfg.path, "/nonexistent"));
    const std::string with = slurp(f);
    CHECK(with.rfind(mine, 0) == 0);  // appended, nothing changed above
    CHECK(phonemic::persistent_mic_installed(cfg.path));
    CHECK(phonemic::set_persistent_mic(false, false, &err, cfg.path, "/nonexistent"));
    CHECK(slurp(f) == mine);  // byte for byte
}

TEST(persistent_mic_pulseaudio_refuses_without_a_system_config) {
    TempDir cfg;
    std::string err;
    CHECK(!phonemic::set_persistent_mic(true, false, &err, cfg.path, cfg.path + "/missing.pa"));
    CHECK(!err.empty());
    CHECK(!fs::exists(cfg.path + "/pulse/default.pa"));
}

#endif
