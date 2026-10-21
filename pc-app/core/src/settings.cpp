#include "phonemic/settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace phonemic {

namespace fs = std::filesystem;

fs::path config_dir() {
    fs::path base;
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) base = fs::path(a) / "PhoneMic";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = fs::path(x) / "phonemic";
    else if (const char* h = std::getenv("HOME")) base = fs::path(h) / ".config/phonemic";
#endif
    if (base.empty()) base = fs::current_path() / ".phonemic";
    std::error_code ec;
    fs::create_directories(base, ec);
#ifndef _WIN32
    ::chmod(base.string().c_str(), 0700);
#endif
    return base;
}

static std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::map<std::string, std::string> parse_ini(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return kv;
}

namespace {
bool to_bool(const std::string& v, bool def) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}
template <typename T>
T to_num(const std::string& v, T def, T lo, T hi) {
    try {
        size_t used = 0;
        const double d = std::stod(v, &used);
        if (used != v.size()) return def;
        if (d < static_cast<double>(lo) || d > static_cast<double>(hi)) return def;
        return static_cast<T>(d);
    } catch (...) {
        return def;
    }
}
}  // namespace

Settings Settings::load(const fs::path& file) {
    Settings s;
    std::ifstream f(file, std::ios::binary);
    if (!f) return s;
    std::stringstream ss;
    ss << f.rdbuf();
    const auto kv = parse_ini(ss.str());
    auto get = [&](const char* k) -> const std::string* {
        auto it = kv.find(k);
        return it == kv.end() ? nullptr : &it->second;
    };
    if (auto v = get("port")) s.port = to_num<uint16_t>(*v, s.port, 1, 65535);
    if (auto v = get("wifi_enabled")) s.wifi_enabled = to_bool(*v, s.wifi_enabled);
    if (auto v = get("remember_passphrase")) s.remember_passphrase = to_bool(*v, s.remember_passphrase);
    if (auto v = get("passphrase"); v && s.remember_passphrase) s.passphrase = *v;
    if (auto v = get("usb_enabled")) s.usb_enabled = to_bool(*v, s.usb_enabled);
    if (auto v = get("voice_focus")) s.voice_focus = to_bool(*v, s.voice_focus);
    if (auto v = get("effect")) s.effect = to_num<int>(*v, s.effect, 0, 255);
    if (auto v = get("gain_db")) s.gain_db = to_num<float>(*v, s.gain_db, -40.0f, 24.0f);
    if (auto v = get("mute")) s.mute = to_bool(*v, s.mute);
    if (auto v = get("monitor")) s.monitor = to_bool(*v, s.monitor);
    if (auto v = get("mic_device")) s.mic_device = *v;
    if (auto v = get("monitor_device")) s.monitor_device = *v;
    if (auto v = get("latency_ms")) s.latency_ms = to_num<int>(*v, s.latency_ms, 10, 500);
    if (auto v = get("theme")) s.theme = to_num<int>(*v, s.theme, 0, 2);
    if (auto v = get("accent")) s.accent = to_num<int>(*v, s.accent, 0, 15);
    return s;
}

bool Settings::save(const fs::path& file) const {
    std::ostringstream o;
    o << "# PhoneMic settings — edited by the app; safe to delete.\n";
    o << "port=" << port << "\n";
    o << "wifi_enabled=" << (wifi_enabled ? 1 : 0) << "\n";
    o << "remember_passphrase=" << (remember_passphrase ? 1 : 0) << "\n";
    o << "passphrase=" << (remember_passphrase ? passphrase : std::string()) << "\n";
    o << "usb_enabled=" << (usb_enabled ? 1 : 0) << "\n";
    o << "voice_focus=" << (voice_focus ? 1 : 0) << "\n";
    o << "effect=" << effect << "\n";
    o << "gain_db=" << gain_db << "\n";
    o << "mute=" << (mute ? 1 : 0) << "\n";
    o << "monitor=" << (monitor ? 1 : 0) << "\n";
    o << "mic_device=" << mic_device << "\n";
    o << "monitor_device=" << monitor_device << "\n";
    o << "latency_ms=" << latency_ms << "\n";
    o << "theme=" << theme << "\n";
    o << "accent=" << accent << "\n";

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
#ifndef _WIN32
        ::chmod(tmp.string().c_str(), 0600);  // private to this user
#endif
        f << o.str();
        if (!f) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
    }
    return !ec;
}

}  // namespace phonemic
