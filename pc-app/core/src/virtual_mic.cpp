#include "phonemic/virtual_mic.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
        const uint32_t idx = pulse->load_module("module-null-sink",
                                                "sink_name=PhoneMic sink_properties=device.description=PhoneMic");
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
        uint32_t idx = pulse->load_module(
            "module-remap-source",
            "master=PhoneMic.monitor source_name=PhoneMic_Mic "
            "source_properties=\"device.description='PhoneMic Microphone' device.icon_name=audio-input-microphone\"");
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
    if (log) log->add("Virtual microphone ready: \"" + vm.mic_name + "\" (" + vm.via + ")");
    return vm;
}

void teardown_virtual_mic() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_loaded_modules.empty()) return;
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
    // Fallback: a third-party virtual cable, if the user already has one.
    for (const auto& d : AudioSystem::get().playback_devices()) {
        if (icontains(d.name, "CABLE Input")) {
            vm.ready = true;
            vm.third_party = true;
            vm.via = "VB-CABLE";
            vm.playback_match = "CABLE Input";
            vm.mic_name = "CABLE Output (VB-Audio Virtual Cable)";
            if (log) log->add("Virtual microphone ready: VB-CABLE (fallback; the PhoneMic driver isn't installed)");
            return vm;
        }
    }
    vm.help = vm.driver_package.empty()
                  ? "This build doesn't include the PhoneMic driver. Use the PhoneMic release zip (it has a "
                    "driver folder), or set up VB-CABLE below."
                  : "Install the PhoneMic driver (button below) so apps can use your phone as a microphone. "
                    "Needs Windows 11 22H2 or newer.";
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

void install_vbcable_async(std::function<void(bool, std::string)> done) {
    std::thread([done = std::move(done)] {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        const std::string result_file = std::string(tmp) + "phonemic_vbcable_result.txt";
        DeleteFileA(result_file.c_str());
        // Download, unpack, verify the Authenticode signature (must be valid and
        // issued to VB-Audio / Vincent Burel), then launch the installer elevated.
        // The outcome is written to a file because the process has no console.
        const std::string script =
            "$ErrorActionPreference='Stop';"
            "$r='" + result_file + "';"
            "try{"
            "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
            "$d=Join-Path $env:TEMP 'phonemic_vbcable';"
            "New-Item -ItemType Directory -Force $d|Out-Null;"
            "$z=Join-Path $d 'vbcable.zip';$ok=$false;"
            "foreach($p in 'VBCABLE_Driver_Pack45.zip','VBCABLE_Driver_Pack43.zip'){"
            "try{Invoke-WebRequest -UseBasicParsing -Uri ('https://download.vb-audio.com/Download_CABLE/'+$p) -OutFile $z;$ok=$true;break}catch{}};"
            "if(-not $ok){throw 'Download failed - check your internet connection'};"
            "Expand-Archive -Path $z -DestinationPath $d -Force;"
            "$exe=Join-Path $d $(if([Environment]::Is64BitOperatingSystem){'VBCABLE_Setup_x64.exe'}else{'VBCABLE_Setup.exe'});"
            "$s=Get-AuthenticodeSignature $exe;"
            "if($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'Burel|VB-Audio'){throw ('Installer signature check failed: '+$s.Status)};"
            "Start-Process -FilePath $exe -Verb RunAs;"
            "Set-Content -Path $r -Value 'OK'"
            "}catch{Set-Content -Path $r -Value ('ERR '+$_.Exception.Message)}";
        std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" +
                           widen(script) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &si, &pi)) {
            done(false, "Couldn't start PowerShell to run the installer.");
            return;
        }
        WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::string res;
        if (FILE* f = std::fopen(result_file.c_str(), "rb")) {
            char buf[1024] = {0};
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            res.assign(buf, n);
        }
        // Set-Content writes UTF-16/ANSI depending on PS version; strip noise.
        std::string clean;
        for (char c : res)
            if (c >= 0x20 && c < 0x7F) clean.push_back(c);
        if (clean.rfind("OK", 0) == 0)
            done(true, "Installer opened. Click \"Install Driver\", then REBOOT and reopen PhoneMic.");
        else if (clean.rfind("ERR", 0) == 0)
            done(false, "Setup failed: " + clean.substr(4));
        else
            done(false, "Setup failed (no result). Install VB-CABLE manually from vb-audio.com/Cable.");
    }).detach();
}

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
            done(false, "Setup failed or was cancelled (see the PowerShell window). VB-CABLE works as a fallback.");
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
    // Fallback: BlackHole, if the user already has it.
    for (const auto& d : devices) {
        if (icontains(d.name, "BlackHole")) {
            vm.ready = true;
            vm.third_party = true;
            vm.via = "BlackHole";
            vm.playback_match = "BlackHole";
            vm.mic_name = d.name;
            if (log) log->add("Virtual microphone ready: " + vm.mic_name + " (fallback; the PhoneMic driver isn't installed)");
            return vm;
        }
    }
    vm.help = vm.driver_package.empty()
                  ? "This build doesn't include PhoneMic.driver. Build it (cmake builds it on macOS) or use "
                    "the release download."
                  : "Install the PhoneMic driver (button below; asks for your password) so apps can use your "
                    "phone as a microphone.";
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
