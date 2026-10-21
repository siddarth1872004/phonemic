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
#include <shellapi.h>
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

#elif defined(_WIN32)

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

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    CableEndpoints cable;
    if (!find_cable(&cable)) {
        vm.can_install = true;
        vm.mic_name = "CABLE Output (VB-Audio Virtual Cable)";
        vm.help = "Windows needs a virtual audio cable for other apps to hear your phone. Install VB-CABLE "
                  "(free, signed, from vb-audio.com) with the button below, then restart Windows.";
        return vm;
    }
    vm.ready = true;
    vm.via = "VB-CABLE";
    // The adapter name stays in the friendly name when the endpoints are renamed.
    vm.playback_match = "VB-Audio Virtual Cable";
    vm.mic_name = cable.mic_name;
    vm.can_rename = !cable.named_phonemic;
    if (log) log->add("Virtual microphone ready: " + cable.mic_name);
    if (cable.default_output) {
        // Everything the PC plays would go into the mic, and you'd hear nothing.
        std::string now;
        if (fix_cable_default_output(&now)) {
            if (log) log->add("VB-CABLE was the default speaker; switched it back to " + now);
        } else if (log) {
            log->add("VB-CABLE is the default speaker: pick your real speakers in Settings > Sound > Output");
        }
    }
    return vm;
}

void teardown_virtual_mic() {}

void install_vbcable_async(std::function<void(bool, std::string)> done) {
    std::thread([done = std::move(done)] {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        const std::string result_file = std::string(tmp) + "phonemic_vbcable_result.txt";
        DeleteFileA(result_file.c_str());
        // Download, unpack, verify the Authenticode signature (must be valid and
        // issued to VB-Audio / Vincent Burel), then launch the installer elevated
        // and wait for it. The outcome is written to a file because the process
        // has no console.
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
            "Start-Process -FilePath $exe -Verb RunAs -Wait;"
            "Set-Content -Path $r -Value 'OK'"
            "}catch{Set-Content -Path $r -Value ('ERR '+$_.Exception.Message)}";
        std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" +
                           widen(script) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                            &pi)) {
            done(false, "Couldn't start PowerShell to run the installer.");
            return;
        }
        WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::string res;
        if (FILE* f = std::fopen(result_file.c_str(), "rb")) {
            char buf[1024] = {0};
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            res.assign(buf, n);
        }
        // Set-Content writes UTF-16/ANSI depending on the PowerShell version.
        std::string clean;
        for (char c : res)
            if (c >= 0x20 && c < 0x7F) clean.push_back(c);
        if (clean.rfind("OK", 0) == 0)
            done(true, "VB-CABLE installer finished. Restart Windows, then reopen PhoneMic.");
        else if (clean.rfind("ERR", 0) == 0)
            done(false, "Setup failed: " + clean.substr(4));
        else
            done(false, "Setup failed (no result). Install VB-CABLE manually from vb-audio.com/Cable.");
    }).detach();
}

void rename_cable_async(bool to_phonemic, std::function<void(bool, std::string)> done) {
    std::thread([to_phonemic, done = std::move(done)] {
        wchar_t exe[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (n == 0 || n >= std::size(exe)) {
            done(false, "Couldn't find PhoneMic.exe to run the rename.");
            return;
        }
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";  // renaming a device needs administrator rights
        sei.lpFile = exe;
        sei.lpParameters = to_phonemic ? L"--rename-cable" : L"--restore-cable";
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            done(false, GetLastError() == ERROR_CANCELLED ? "Rename cancelled." : "Couldn't start the rename.");
            return;
        }
        WaitForSingleObject(sei.hProcess, 60 * 1000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code == 0)
            done(true, to_phonemic ? "Renamed: apps now list \"PhoneMic Microphone\". Restart apps that were open."
                                   : "Restored VB-CABLE's own names.");
        else if (code == 3)
            done(false, "Rename failed: VB-CABLE isn't installed or is disabled.");
        else
            done(false, "Rename failed: Windows refused to change the device name.");
    }).detach();
}

#else

VirtualMic setup_virtual_mic(EventLog*) {
    VirtualMic vm;
    vm.help = "No virtual microphone support on this platform.";
    return vm;
}
void teardown_virtual_mic() {}

#endif

}  // namespace phonemic
