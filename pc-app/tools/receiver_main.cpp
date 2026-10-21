// phonemic-receiver — the PhoneMic receive pipeline without a window.
//
// Same engine as the desktop app (Bluetooth, Wi-Fi and USB links, Voice Focus,
// voice changer, virtual mic), for headless boxes and scripting.
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <algorithm>

#include "phonemic/audio_out.hpp"
#include "phonemic/bluetooth.hpp"
#include "phonemic/net.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/usb.hpp"
#include "phonemic/util.hpp"
#include "phonemic/virtual_mic.hpp"
#include "phonemic/wifi.hpp"

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
        "  --port N            Wi-Fi UDP / USB TCP port (default 4010, or $PHONEMIC_PORT)\n"
        "  --pin TEXT          Wi-Fi: accept only audio encrypted with this PIN\n"
        "  --no-wifi           don't listen on the network\n"
        "  --no-bluetooth      don't publish the Bluetooth service\n"
        "  --usb               accept a phone on a USB cable (runs adb reverse)\n"
        "  --no-focus          disable Voice Focus noise removal\n"
        "  --effect NAME       normal|deep|chipmunk|robot|radio|echo\n"
        "  --gain DB           output gain in dB (-40..24, limiter prevents clipping)\n"
        "  --latency MS        playout buffer target (default 40)\n"
        "  --device TEXT       output device (name or part of it); default: virtual mic\n"
        "  --monitor TEXT      also play on this device (\"default\" = system default)\n"
        "  --list-devices      list output devices and exit\n"
        "  --no-virtual-mic    don't create/use the virtual microphone\n"
        "  --null              discard audio (no sound device; for testing)\n"
#ifdef _WIN32
        "  --rename-cable      name VB-CABLE's endpoints \"PhoneMic Microphone\" / \"PhoneMic\n"
        "                      Feed\" (run as administrator), then exit\n"
        "  --restore-cable     give VB-CABLE's endpoints their own names back, then exit\n"
#endif
        "  --make-default      make PhoneMic the system's default microphone and exit\n"
#if defined(__linux__)
        "  --keep-mic on|off   keep \"PhoneMic Microphone\" registered when PhoneMic is\n"
        "                      closed (sound-server config), then exit\n"
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
    uint16_t port = PM_DEFAULT_PORT;
    if (const char* e = std::getenv("PHONEMIC_PORT")) port = static_cast<uint16_t>(std::atoi(e));
    std::string pin, device, monitor;
    bool focus = true, list = false, use_vmic = true, null_out = false;
    bool wifi_on = true, bt_on = true, usb = false;
    int latency = 40;
    float gain = 0;
    Effect effect = Effect::None;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--pin") pin = next();
        else if (a == "--no-wifi") wifi_on = false;
        else if (a == "--no-bluetooth") bt_on = false;
        else if (a == "--usb") usb = true;
        else if (a == "--no-focus") focus = false;
        else if (a == "--effect") effect = parse_effect(next());
        else if (a == "--gain") gain = std::stof(next());
        else if (a == "--latency") latency = std::stoi(next());
        else if (a == "--device") device = next();
        else if (a == "--monitor") monitor = next();
        else if (a == "--list-devices") list = true;
        else if (a == "--no-virtual-mic") use_vmic = false;
        else if (a == "--null") null_out = true;
#ifdef _WIN32
        else if (a == "--rename-cable" || a == "--restore-cable") {
            const int rc = rename_cable_now(a == "--rename-cable");
            if (rc != 0) std::fprintf(stderr, rc == 3 ? "VB-CABLE isn't installed.\n"
                                                      : "Windows refused (run this as administrator).\n");
            return rc;
        }
#endif
        else if (a == "--version") { std::puts("phonemic-receiver " PHONEMIC_VERSION); return 0; }
        else if (a == "--make-default") {
            EventLog l(20, true);
            const VirtualMic vm = setup_virtual_mic(&l);
            std::string err;
            const bool ok = vm.ready && make_default_mic(vm, &err);
            if (!ok) std::fprintf(stderr, "Not changed: %s\n", vm.ready ? err.c_str() : vm.help.c_str());
            else std::printf("\"%s\" is now the default microphone.\n", vm.mic_name.c_str());
            // Keep the device the default points at (Linux removes it on exit
            // unless --keep-mic is on).
            return ok ? 0 : 1;
        }
#if defined(__linux__)
        else if (a == "--keep-mic") {
            const std::string v = next();
            if (v != "on" && v != "off") { usage(); return 2; }
            const VirtualMic vm = setup_virtual_mic(nullptr);
            std::string err;
            const bool ok = vm.can_persist && set_persistent_mic(v == "on", vm.via == "PipeWire", &err);
            if (!ok) std::fprintf(stderr, "Not changed: %s\n", vm.can_persist ? err.c_str() : vm.help.c_str());
            else std::printf(v == "on" ? "PhoneMic Microphone stays registered when PhoneMic is closed.\n"
                                       : "PhoneMic Microphone is removed when PhoneMic closes.\n");
            teardown_virtual_mic();
            return ok ? 0 : 1;
        }
#endif
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
    rx.set_voice_focus(focus);
    rx.set_effect(effect);
    rx.set_gain_db(gain);
    rx.set_passphrase(pin);

    NullSink null_sink;
    AudioOutput out, mon;
    std::string err;
    std::string out_name = "nothing (--null)";
    if (null_out) {
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

    BluetoothServer bt(&rx, &log);
    if (bt_on) bt.start();
    WifiServer wifi(&rx, &log);
    if (wifi_on && !wifi.start(port, &err)) {
        std::fprintf(stderr, "Wi-Fi: %s\n(Is another PhoneMic already running? Try --port.)\n", err.c_str());
        wifi_on = false;
    }
    UsbServer usb_in(&rx, &log);
    AdbBridge adb(&log);
    if (usb) {
        if (!usb_in.start(port, &err)) std::fprintf(stderr, "USB: %s\n", err.c_str());
        else adb.start(usb_in.port());
    }

    std::printf("=====================================================\n");
    std::printf(" PhoneMic receiver %s — ready\n", PHONEMIC_VERSION);
    std::printf("=====================================================\n");
    if (bt_on) std::printf(" Bluetooth: pair the phone with this PC, pick it in the app\n");
    if (wifi_on) {
        std::printf(" Wi-Fi: same network, port %u%s", wifi.port(), pin.empty() ? "" : " (PIN required)");
        for (const auto& a : net::local_addresses()) std::printf("  %s", a.ip.c_str());
        std::printf("\n");
    }
    if (usb) std::printf(" USB: plug the phone in (USB debugging on), pick USB in the app\n");
    std::printf(" Audio -> %s\n", out_name.c_str());
    if (vmic.ready) std::printf(" In Discord/Zoom/OBS pick the microphone: \"%s\"\n", vmic.mic_name.c_str());
    else if (!vmic.help.empty()) std::printf(" Note: %s\n", vmic.help.c_str());
    std::printf(" Ctrl-C to stop.\n-----------------------------------------------------\n");
    std::fflush(stdout);

    uint64_t last_print = mono_ms();
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    wifi.stop();
    bt.stop();
    teardown_virtual_mic();
    return 0;
}
