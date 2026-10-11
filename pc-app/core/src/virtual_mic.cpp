#include "phonemic/virtual_mic.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "phonemic/audio_out.hpp"
#include "phonemic/process.hpp"
#include "phonemic/pulse_ctl.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>

#include "driver_feed.hpp"
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace phonemic {

#if defined(__linux__)

namespace {
std::vector<uint32_t> g_loaded_modules;
std::mutex g_mu;

bool file_contains(const char* path, const std::string& needle) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[512];
    bool found = false;
    while (!found && std::fgets(buf, sizeof buf, f)) found = std::string(buf).find(needle) != std::string::npos;
    std::fclose(f);
    return found;
}

// ---- persistent registration (sound-server configuration) ----------------------

constexpr char kNullSinkArgs[] = "sink_name=PhoneMic sink_properties=device.description=PhoneMic";
// The whole property list is one quoted module argument, with the description
// quoted again inside it so it keeps its space.
constexpr char kRemapArgs[] =
    "master=PhoneMic.monitor source_name=PhoneMic_Mic "
    "source_properties=\"device.description='PhoneMic Microphone' device.icon_name=audio-input-microphone\"";
constexpr char kPaBegin[] = "### PhoneMic begin";
constexpr char kPaEnd[] = "### PhoneMic end";
constexpr char kPaCreatedBy[] = "# Created by PhoneMic";

std::string config_dir(const std::string& override_home) {
    if (!override_home.empty()) return override_home;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return x;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config";
}
std::string pipewire_dropin(const std::string& cfg) {
    return cfg + "/pipewire/pipewire-pulse.conf.d/90-phonemic.conf";
}
std::string pulse_user_pa(const std::string& cfg) { return cfg + "/pulse/default.pa"; }

bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}
// Write via a temporary file + rename, so a crash never leaves half a config
// that could stop the sound server from starting.
bool write_file(const std::string& path, const std::string& text) {
    const auto slash = path.rfind('/');
    std::error_code ec;
    std::filesystem::create_directories(path.substr(0, slash), ec);
    const std::string tmp = path + ".phonemic-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.flush()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string pipewire_dropin_text() {
    // SPA JSON: the inner double quotes are escaped.
    std::string remap = kRemapArgs;
    std::string esc;
    for (char c : remap) {
        if (c == '"') esc += '\\';
        esc += c;
    }
    return std::string(
               "# Created by PhoneMic: keeps \"PhoneMic Microphone\" available when PhoneMic is closed.\n"
               "# Untick the option in PhoneMic (or delete this file) to remove it.\n"
               "pulse.cmd = [\n"
               "    { cmd = \"load-module\" args = \"module-null-sink ") +
           kNullSinkArgs +
           "\" flags = [ \"nofail\" ] }\n"
           "    { cmd = \"load-module\" args = \"module-remap-source " +
           esc + "\" flags = [ \"nofail\" ] }\n]\n";
}

std::string pulse_block() {
    // .nofail: if a module ever fails to load, PulseAudio must still start.
    return std::string(kPaBegin) + " (keeps \"PhoneMic Microphone\" available when PhoneMic is closed)\n" +
           ".nofail\n" + "load-module module-null-sink " + kNullSinkArgs + "\n" + "load-module module-remap-source " +
           kRemapArgs + "\n" + ".fail\n" + kPaEnd + "\n";
}

// Remove our marked block (and the blank line before it) from a default.pa.
std::string without_pulse_block(const std::string& text) {
    const auto b = text.find(kPaBegin);
    if (b == std::string::npos) return text;
    auto e = text.find(kPaEnd, b);
    e = (e == std::string::npos) ? text.size() : text.find('\n', e);
    e = (e == std::string::npos) ? text.size() : e + 1;
    auto start = b;
    if (start >= 1 && text[start - 1] == '\n' && start >= 2 && text[start - 2] == '\n') --start;
    return text.substr(0, start) + text.substr(e);
}

// No sound server at all (plain ALSA): the kernel's loopback card, if loaded.
// Audio played into its device 0 can be recorded from its device 1.
VirtualMic alsa_loopback(const std::string& why, EventLog* log) {
    VirtualMic vm;
    if (file_contains("/proc/asound/cards", "Loopback")) {
        vm.ready = true;
        vm.via = "ALSA loopback";
        vm.playback_match = "Loopback";
        vm.mic_name = "Loopback (device 1)  [hw:Loopback,1]";
        if (log) log->add("Virtual microphone ready: ALSA loopback (snd-aloop), " + why);
        return vm;
    }
    vm.help = "No PulseAudio/PipeWire sound server found (" + why +
              "). Install pipewire-pulse (or pulseaudio) for a \"PhoneMic Microphone\", or on plain ALSA "
              "load the loopback card: sudo modprobe snd-aloop, then restart PhoneMic.";
    return vm;
}
}  // namespace

bool persistent_mic_installed(const std::string& config_home) {
    const std::string cfg = config_dir(config_home);
    std::string text;
    if (read_file(pipewire_dropin(cfg), &text)) return true;
    return read_file(pulse_user_pa(cfg), &text) && text.find(kPaBegin) != std::string::npos;
}

bool set_persistent_mic(bool on, bool pipewire, std::string* err, const std::string& config_home,
                        const std::string& system_default_pa) {
    const std::string cfg = config_dir(config_home);
    const std::string pa = pulse_user_pa(cfg);
    if (!on) {
        std::error_code ec;
        std::filesystem::remove(pipewire_dropin(cfg), ec);
        std::string text;
        if (read_file(pa, &text) && text.find(kPaBegin) != std::string::npos) {
            const std::string rest = without_pulse_block(text);
            // A file PhoneMic created holds nothing else: remove it entirely.
            if (rest.rfind(kPaCreatedBy, 0) == 0 && rest.find("load-module") == std::string::npos)
                std::filesystem::remove(pa, ec);
            else if (!write_file(pa, rest)) {
                if (err) *err = "couldn't write " + pa;
                return false;
            }
        }
        return true;
    }
    if (pipewire) {
        if (!write_file(pipewire_dropin(cfg), pipewire_dropin_text())) {
            if (err) *err = "couldn't write " + pipewire_dropin(cfg);
            return false;
        }
        return true;
    }
    std::string text;
    if (read_file(pa, &text)) {
        if (text.find(kPaBegin) != std::string::npos) return true;  // already there
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += "\n" + pulse_block();
    } else {
        // A user default.pa replaces the system one, so it must include it;
        // without a system file to include, don't touch anything.
        std::string sys;
        if (!read_file(system_default_pa, &sys)) {
            if (err) *err = "couldn't find the system PulseAudio configuration (" + system_default_pa + ")";
            return false;
        }
        text = std::string(kPaCreatedBy) + ": your system's sound setup, plus PhoneMic's microphone.\n" +
               ".include " + system_default_pa + "\n\n" + pulse_block();
    }
    if (!write_file(pa, text)) {
        if (err) *err = "couldn't write " + pa;
        return false;
    }
    return true;
}

VirtualMic setup_virtual_mic(EventLog* log) {
    std::string why;
    auto pulse = PulseCtl::connect(&why);
    if (!pulse) return alsa_loopback(why, log);

    VirtualMic vm;
    vm.via = pulse->server_name().find("PipeWire") != std::string::npos ? "PipeWire" : "PulseAudio";
    vm.playback_match = "PhoneMic";
    vm.mic_name = "PhoneMic Microphone";
    std::lock_guard<std::mutex> lk(g_mu);
    if (!pulse->has_sink("PhoneMic")) {
        const uint32_t idx = pulse->load_module("module-null-sink", kNullSinkArgs);
        if (idx == PulseCtl::kInvalid) {
            vm.help = "The sound server refused to create the PhoneMic sink (module-null-sink).";
            return vm;
        }
        g_loaded_modules.push_back(idx);
    }
    if (!pulse->has_source("PhoneMic_Mic")) {
        // The whole property list is one quoted module argument, with the
        // description quoted again inside it so it keeps its space. (The 0.3
        // pre-release quoted only the description: PulseAudio rejected the
        // module, so the "PhoneMic Microphone" source never appeared.)
        uint32_t idx = pulse->load_module("module-remap-source", kRemapArgs);
        if (idx == PulseCtl::kInvalid) {
            // Servers that reject the property list: same source, default name.
            idx = pulse->load_module("module-remap-source", "master=PhoneMic.monitor source_name=PhoneMic_Mic");
            vm.mic_name = idx == PulseCtl::kInvalid ? "Monitor of PhoneMic" : "Remapped Monitor of PhoneMic";
        }
        if (idx != PulseCtl::kInvalid) g_loaded_modules.push_back(idx);
    }
    // ALSA-backend fallback: the pulse ALSA plugin honours PULSE_SINK.
    ::setenv("PULSE_SINK", "PhoneMic", 1);
    vm.ready = true;
    vm.can_persist = true;
    vm.persistent = persistent_mic_installed();
    if (log) log->add("Virtual microphone ready: \"" + vm.mic_name + "\" (" + vm.via + ")");
    return vm;
}

void teardown_virtual_mic() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_loaded_modules.empty()) return;
    // Kept on purpose: the sound server recreates it at login anyway.
    if (persistent_mic_installed()) {
        g_loaded_modules.clear();
        return;
    }
    if (auto pulse = PulseCtl::connect(nullptr)) {
        // Reverse order: the remapped source depends on the sink's monitor.
        for (auto it = g_loaded_modules.rbegin(); it != g_loaded_modules.rend(); ++it) pulse->unload_module(*it);
    }
    g_loaded_modules.clear();
}

void install_driver_async(const VirtualMic&, std::function<void(bool, std::string)> done) {
    done(false, "Nothing to install on Linux: PulseAudio/PipeWire provide the virtual microphone.");
}

#elif defined(_WIN32)

namespace {
std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// The release zip carries the driver in driver\ next to PhoneMic.exe.
std::string find_driver_package() {
    const std::wstring dir = exe_dir() + L"\\driver";
    const DWORD a = GetFileAttributesW((dir + L"\\install-driver.ps1").c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? narrow(dir) : std::string();
}
}  // namespace

namespace {
// ACX + KMDF 1.33: Windows 11 22H2.
constexpr DWORD kMinWindowsBuild = 22621;

// Build number from the kernel (GetVersionEx lies to unmanifested apps).
DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))))
            if (fn(&v) == 0) return v.dwBuildNumber;
    return 0;
}

// Is the PhoneMic device (Root\PhonemicCapture) installed, running or not?
bool driver_device_installed() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t ids[512] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                              sizeof ids - sizeof(wchar_t), nullptr))
            for (const wchar_t* p = ids; *p && !found; p += wcslen(p) + 1)
                found = _wcsicmp(p, L"Root\\PhonemicCapture") == 0;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
}  // namespace

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    vm.driver_package = find_driver_package();
    vm.mic_name = "PhoneMic Microphone";
    if (DriverFeed::available()) {
        vm.ready = true;
        vm.direct_feed = true;
        vm.via = "PhoneMic driver";
        if (log) log->add("Virtual microphone ready: PhoneMic driver");
        return vm;
    }
    const DWORD build = windows_build();
    if (build != 0 && build < kMinWindowsBuild) {
        vm.driver_package.clear();  // nothing installable on this Windows
        vm.help = "PhoneMic's microphone driver needs Windows 11, version 22H2 or newer (this PC runs build " +
                  std::to_string(build) + "). Update Windows to use your phone as a microphone.";
        return vm;
    }
    if (driver_device_installed()) {
        vm.help = "The PhoneMic driver is installed but not running yet. Restart Windows. If it still doesn't "
                  "start, test mode is off: click Install PhoneMic driver again.";
        return vm;
    }
    vm.help = vm.driver_package.empty()
                  ? "This copy of PhoneMic has no driver folder next to PhoneMic.exe. Download the release zip "
                    "(PhoneMic-windows-x64.zip) and run PhoneMic from it."
                  : "Install the PhoneMic driver (button below) so every app can use your phone as a microphone.";
    return vm;
}

void teardown_virtual_mic() {}

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
}  // namespace

// Runs driver\\install-driver.ps1 elevated in a visible console: it explains
// and asks before changing anything (test-signing needs the user's consent).
// Exit codes: 0 installed and running, 3010 installed but Windows must restart.
void install_driver_async(const VirtualMic& vm, std::function<void(bool, std::string)> done) {
    const std::wstring pkg = widen(vm.driver_package);
    std::thread([pkg, done = std::move(done)] {
        if (pkg.empty()) {
            done(false, "Setup failed: this build has no driver folder next to PhoneMic.exe.");
            return;
        }
        const std::wstring params =
            L"-NoProfile -ExecutionPolicy Bypass -File \"" + pkg + L"\\install-driver.ps1\" -FromApp";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = params.c_str();
        sei.lpDirectory = pkg.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            done(false, GetLastError() == ERROR_CANCELLED ? "Setup failed: cancelled."
                                                          : "Setup failed: couldn't start PowerShell.");
            return;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code == 0)
            done(true, "PhoneMic driver installed.");
        else if (code == 3010)
            done(true, "PhoneMic driver installed. Restart Windows to finish (test mode was just enabled).");
        else
            done(false, "Setup failed or was cancelled (see the PowerShell window for the reason).");
    }).detach();
}

#elif defined(__APPLE__)

namespace {
std::string exe_dir() {
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::string p(buf);
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

bool is_dir(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// PhoneMic.app/Contents/Resources/PhoneMic.driver (the build copies it there),
// or PhoneMic.driver next to a bare binary.
std::string find_driver_package() {
    const std::string dir = exe_dir();
    for (const std::string& p : {dir + "/../Resources/PhoneMic.driver", dir + "/PhoneMic.driver"}) {
        const auto slash = p.rfind('/');
        if (is_dir(p) && ::access((p.substr(0, slash) + "/install-driver.sh").c_str(), R_OK) == 0) return p;
    }
    return {};
}
}  // namespace

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    vm.driver_package = find_driver_package();
    const auto devices = AudioSystem::get().playback_devices();
    for (const auto& d : devices) {
        if (icontains(d.name, "PhoneMic Microphone")) {
            vm.ready = true;
            vm.via = "PhoneMic driver";
            vm.playback_match = "PhoneMic Microphone";
            vm.mic_name = "PhoneMic Microphone";
            if (log) log->add("Virtual microphone ready: PhoneMic driver");
            return vm;
        }
    }
    vm.help = vm.driver_package.empty()
                  ? "This build doesn't include PhoneMic.driver. Build it (cmake builds it on macOS) or use "
                    "the release download."
                  : "Install the PhoneMic driver (button below; asks for your password) so every app can use "
                    "your phone as a microphone.";
    return vm;
}

void teardown_virtual_mic() {}

void install_driver_async(const VirtualMic& vm, std::function<void(bool, std::string)> done) {
    const std::string pkg = vm.driver_package;
    std::thread([pkg, done = std::move(done)] {
        if (pkg.empty()) {
            done(false, "Setup failed: PhoneMic.driver isn't bundled with this build.");
            return;
        }
        const std::string script = pkg.substr(0, pkg.rfind('/')) + "/install-driver.sh";
        // macOS shows its own administrator prompt. Paths go in as arguments
        // (quoted form), never spliced into the script text.
        const ProcessResult r = run_process(
            {"/usr/bin/osascript", "-e", "on run argv",
             "-e",
             "do shell script \"/bin/sh \" & quoted form of (item 1 of argv) & \" \" & "
             "quoted form of (item 2 of argv) with administrator privileges",
             "-e", "end run", script, pkg},
            180000);
        if (r.started && r.exit_code == 0) {
            // coreaudiod restarts and loads the plug-in: give it a moment.
            std::this_thread::sleep_for(std::chrono::seconds(2));
            done(true, "PhoneMic driver installed.");
        } else if (r.output.find("-128") != std::string::npos) {
            done(false, "Setup failed: cancelled.");
        } else {
            done(false, "Setup failed: " + (r.output.empty() ? std::string("osascript did not run") : r.output));
        }
    }).detach();
}

#else

VirtualMic setup_virtual_mic(EventLog*) {
    VirtualMic vm;
    vm.help = "No virtual microphone support on this platform.";
    return vm;
}
void teardown_virtual_mic() {}
void install_driver_async(const VirtualMic&, std::function<void(bool, std::string)> done) {
    done(false, "Not supported on this platform.");
}

#endif

}  // namespace phonemic
