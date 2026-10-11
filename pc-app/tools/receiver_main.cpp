// phonemic-receiver — the PhoneMic receive pipeline without a window.
//
// Same engine as the desktop app (Voice Focus, voice changer, virtual mic,
// browser mic), for headless boxes, scripting and latency measurement.
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include "phonemic/audio_out.hpp"
#include "phonemic/net.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/usb.hpp"
#include "phonemic/util.hpp"
#include "phonemic/virtual_mic.hpp"
#ifdef PHONEMIC_HAVE_WEB
#include "phonemic/web_server.hpp"
#endif
#ifdef _WIN32
#include "driver_feed.hpp"
#endif

using namespace phonemic;

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }

struct NullSink : AudioSinkIface {
    void write(const float*, size_t) override {}
};

void usage() {
    std::puts(
        "usage: phonemic-receiver [options]\n"
        "  --port N            UDP port (default 4010, or $PHONEMIC_PORT)\n"
        "  --pin TEXT          require this PIN/passphrase (encrypted audio only)\n"
        "  --no-focus          disable Voice Focus noise removal\n"
        "  --effect NAME       normal|deep|chipmunk|robot|radio|echo\n"
        "  --gain DB           output gain in dB (-40..24, limiter prevents clipping)\n"
        "  --latency MS        playout buffer target (default 40)\n"
        "  --device TEXT       output device (name or part of it); default: virtual mic\n"
        "  --monitor TEXT      also play on this device (\"default\" = system default)\n"
        "  --list-devices      list output devices and exit\n"
        "  --no-virtual-mic    don't create/use the virtual microphone\n"
        "  --null              discard audio (no sound device; for testing)\n"
        "  --web [PORT]        also serve the browser mic (HTTPS, default port 8443)\n"
        "  --usb               accept a phone on a USB cable (runs adb reverse)\n"
        "  --measure [N]       print one-way latency stats for N packets (use with\n"
        "                      phonemic-softphone --wallclock on the same machine)\n"
#ifdef _WIN32
        "  --driver            require the PhoneMic driver (default: use it if installed)\n"
#endif
        "  --version");
}

Effect parse_effect(const std::string& s) {
    for (int i = 0; i < kEffectCount; ++i) {
        std::string n = effect_name(static_cast<Effect>(i));
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n == s || (s == "none" && i == 0)) return static_cast<Effect>(i);
    }
    std::fprintf(stderr, "unknown effect '%s', using normal\n", s.c_str());
    return Effect::None;
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 4010;
    if (const char* e = std::getenv("PHONEMIC_PORT")) port = static_cast<uint16_t>(std::atoi(e));
    std::string pin, device, monitor;
    bool focus = true, list = false, use_vmic = true, null_out = false, web = false, driver = false, usb = false;
    uint16_t web_port = 8443;
    int latency = 40;
    float gain = 0;
    Effect effect = Effect::None;
    long measure = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        auto has_num_next = [&] { return i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9'; };
        if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--pin") pin = next();
        else if (a == "--no-focus") focus = false;
        else if (a == "--effect") effect = parse_effect(next());
        else if (a == "--gain") gain = std::stof(next());
        else if (a == "--latency") latency = std::stoi(next());
        else if (a == "--device") device = next();
        else if (a == "--monitor") monitor = next();
        else if (a == "--list-devices") list = true;
        else if (a == "--no-virtual-mic") use_vmic = false;
        else if (a == "--null") null_out = true;
        else if (a == "--web") { web = true; if (has_num_next()) web_port = static_cast<uint16_t>(std::stoi(next())); }
        else if (a == "--measure") { measure = 300; if (has_num_next()) measure = std::stol(next()); }
        else if (a == "--driver") driver = true;
        else if (a == "--usb") usb = true;
        else if (a == "--version") { std::puts("phonemic-receiver " PHONEMIC_VERSION); return 0; }
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    if (list) {
        for (const auto& d : AudioSystem::get().playback_devices())
            std::printf("%s %s\n", d.is_default ? "*" : " ", d.name.c_str());
        std::printf("(backend: %s)\n", AudioSystem::get().backend_name().c_str());
        return 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    EventLog log(60, /*echo_stderr=*/true);
    VirtualMic vmic;
    if (use_vmic && !null_out) vmic = setup_virtual_mic(&log);

    Receiver rx(&log);
    rx.set_passphrase(pin);
    rx.set_voice_focus(focus);
    rx.set_effect(effect);
    rx.set_gain_db(gain);

    NullSink null_sink;
    AudioOutput out, mon;
    std::unique_ptr<AudioSinkIface> driver_sink;
    std::string err;
    std::string out_name = "nothing (--null)";
    if (driver || (device.empty() && !null_out && !measure && vmic.direct_feed)) {
#ifdef _WIN32
        auto f = DriverFeed::open(&err);
        if (!f) { std::fprintf(stderr, "PhoneMic driver: %s\n", err.c_str()); return 1; }
        f->set_latency_ms(static_cast<uint32_t>(latency));
        driver_sink = std::move(f);
        rx.set_mic_sink(driver_sink.get());
        out_name = "PhoneMic driver (apps record \"PhoneMic Microphone\")";
#else
        std::fprintf(stderr, "--driver is Windows-only\n");
        return 2;
#endif
    } else if (null_out || measure) {
        rx.set_mic_sink(&null_sink);
    } else {
        const std::string want = !device.empty() ? device : (vmic.ready ? vmic.playback_match : "");
        out.set_latency_ms(static_cast<uint32_t>(latency));
        bool ok = out.open(want, &err);
        if (!ok && !want.empty()) ok = out.open("", &err);  // fall back to the default device
        if (!ok) {
            std::fprintf(stderr, "No audio output: %s\n", err.c_str());
            return 1;
        }
        if (!device.empty() && !out.matched())
            std::fprintf(stderr, "No output device matches \"%s\"; using the default.\n", device.c_str());
        rx.set_mic_sink(&out);
        out_name = out.device_name();
        if (!monitor.empty()) {
            mon.set_latency_ms(static_cast<uint32_t>(latency));
            if (mon.open(monitor == "default" ? "" : monitor, &err)) {
                rx.set_monitor_sink(&mon);
                rx.set_monitor(true);
            } else {
                std::fprintf(stderr, "monitor: %s\n", err.c_str());
            }
        }
    }

    std::vector<double> lat_ms;
    if (measure) {
        lat_ms.reserve(static_cast<size_t>(measure));
        rx.set_packet_observer([&](const pm_audio_header& h, uint64_t arrival) {
            if (static_cast<long>(lat_ms.size()) < measure && arrival >= h.timestamp_us)
                lat_ms.push_back(static_cast<double>(arrival - h.timestamp_us) / 1000.0);
        });
    }

    if (!rx.start(port, &err)) {
        std::fprintf(stderr, "%s\n(Is another PhoneMic already running? Try --port.)\n", err.c_str());
        return 1;
    }

    UsbIngest usb_in(&rx, &log);
    AdbBridge adb(&log);
    if (usb) {
        if (!usb_in.start(port, &err)) std::fprintf(stderr, "USB: %s\n", err.c_str());
        adb.start(port);
    }

#ifdef PHONEMIC_HAVE_WEB
    WebServer webserver(&rx, &log);
    std::string token;
    if (web) {
        token = random_token(8);
        if (!webserver.start(web_port, token, &err)) std::fprintf(stderr, "browser mic: %s\n", err.c_str());
    }
#else
    if (web) std::fprintf(stderr, "this build has no browser-mic support\n");
#endif

    std::printf("=====================================================\n");
    std::printf(" PhoneMic receiver %s — ready\n", PHONEMIC_VERSION);
    std::printf("=====================================================\n");
    std::printf(" On your phone, tap \"Find PC\" or enter one of:\n");
    for (const auto& a : net::local_addresses())
        std::printf("     %-16s (%s)%s\n", a.ip.c_str(), a.interface_name.c_str(), a.primary ? "  <- likely" : "");
    std::printf(" port %u%s\n", rx.port(), pin.empty() ? "" : "   (PIN required)");
#ifdef PHONEMIC_HAVE_WEB
    if (web && webserver.running()) {
        const auto addrs = net::local_addresses();
        std::printf(" Browser mic: %s\n",
                    WebServer::url_for(addrs.empty() ? "<pc-ip>" : addrs[0].ip, web_port, token).c_str());
    }
#endif
    if (usb) std::printf(" USB: plug the phone in (USB debugging on), enable \"USB cable\" in the app\n");
    std::printf(" Audio -> %s\n", out_name.c_str());
    if (vmic.ready) std::printf(" In Discord/Zoom/OBS pick the microphone: \"%s\"\n", vmic.mic_name.c_str());
    else if (!vmic.help.empty()) std::printf(" Note: %s\n", vmic.help.c_str());
    std::printf(" Ctrl-C to stop.\n-----------------------------------------------------\n");
    std::fflush(stdout);

    uint64_t last_print = mono_ms();
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (measure && static_cast<long>(lat_ms.size()) >= measure) break;
        if (mono_ms() - last_print >= 5000) {
            last_print = mono_ms();
            const auto s = rx.snapshot();
            if (s.connected) {
                const double loss = s.packets + s.lost ? 100.0 * s.lost / (s.packets + s.lost) : 0.0;
                std::printf("[%s] %llu packets, %.2f%% lost, jitter %.1f ms, buffer %u ms\n", s.peer.c_str(),
                            static_cast<unsigned long long>(s.packets), loss, s.jitter_ms, s.mic_buffer_ms);
                std::fflush(stdout);
            }
        }
    }
    adb.stop();
    usb_in.stop();
    rx.stop();
    teardown_virtual_mic();

    if (measure && !lat_ms.empty()) {
        std::sort(lat_ms.begin(), lat_ms.end());
        double mean = 0;
        for (double v : lat_ms) mean += v;
        mean /= static_cast<double>(lat_ms.size());
        double var = 0;
        for (double v : lat_ms) var += (v - mean) * (v - mean);
        auto pct = [&](double p) { return lat_ms[static_cast<size_t>((lat_ms.size() - 1) * p)]; };
        std::printf("measured %zu packets\none-way latency (ms): min %.3f  median %.3f  p95 %.3f  max %.3f\n",
                    lat_ms.size(), lat_ms.front(), pct(0.5), pct(0.95), lat_ms.back());
        std::printf("  mean %.3f  jitter (stddev) %.3f\n", mean, std::sqrt(var / lat_ms.size()));
        std::printf("note: transport/software latency only; add the playout buffer (%d ms) and,\n"
                    "with a real phone, capture + Wi-Fi time.\n", latency);
    }
    return 0;
}
