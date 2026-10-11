#include "phonemic/settings.hpp"

#include <filesystem>
#include <fstream>

#include "phonemic/util.hpp"
#include "test.hpp"

using namespace phonemic;
namespace fs = std::filesystem;

TEST(settings_parse_ini_tolerates_noise) {
    const auto kv = parse_ini("# comment\n[section]\n port = 5000 \nbad line\n;x=y\nkey=a=b\n");
    CHECK(kv.at("port") == "5000");
    CHECK(kv.at("key") == "a=b");
    CHECK(kv.count(";x") == 0);
}

TEST(settings_round_trip_and_bad_values_keep_defaults) {
    const fs::path dir = fs::temp_directory_path() / ("phonemic-test-" + random_token(8));
    fs::create_directories(dir);
    const fs::path f = dir / "settings.ini";
    Settings s;
    s.port = 5001;
    s.passphrase = "open sesame";
    s.voice_focus = false;
    s.effect = 3;
    s.gain_db = 4.5f;
    s.mic_device = "Speakers (Realtek(R) Audio)";
    s.latency_ms = 80;
    s.web_enabled = true;
    s.web_token = "abc";
    CHECK(s.save(f));
    const auto l = Settings::load(f);
    CHECK_EQ(l.port, 5001);
    CHECK(l.passphrase == "open sesame");
    CHECK(!l.voice_focus);
    CHECK_EQ(l.effect, 3);
    CHECK_NEAR(l.gain_db, 4.5, 1e-6);
    CHECK(l.mic_device == s.mic_device);
    CHECK_EQ(l.latency_ms, 80);
    CHECK(l.web_enabled);
    CHECK(l.web_token == "abc");

    std::ofstream(f) << "port=99999\nlatency_ms=abc\ngain_db=1e9\n";
    const auto d = Settings::load(f);
    CHECK_EQ(d.port, 4010);
    CHECK_EQ(d.latency_ms, 40);
    CHECK_NEAR(d.gain_db, 0.0, 1e-9);

    Settings forget = s;
    forget.remember_passphrase = false;
    CHECK(forget.save(f));
    CHECK(Settings::load(f).passphrase.empty());
    fs::remove_all(dir);
    CHECK(Settings::load(dir / "missing.ini").port == 4010);
}
