#include "phonemic/virtual_mic.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "phonemic/audio_out.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace phonemic {

#if defined(__linux__)

namespace {
std::vector<std::string> g_loaded_modules;
std::mutex g_mu;

// Run a fixed command line (no user input is ever interpolated) and capture stdout.
bool run(const std::string& cmd, std::string* out) {
    FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return false;
    char buf[512];
    std::string s;
    while (std::fgets(buf, sizeof buf, p)) s += buf;
    const int rc = ::pclose(p);
    if (out) *out = s;
    return rc == 0;
}

bool has_column(const std::string& listing, const std::string& name) {
    std::istringstream in(listing);
    std::string line;
    while (std::getline(in, line)) {
        // "index<TAB>name<TAB>..."
        const auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const auto t2 = line.find('\t', t1 + 1);
        if (line.substr(t1 + 1, t2 - t1 - 1) == name) return true;
    }
    return false;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    vm.playback_match = "PhoneMic";
    vm.mic_name = "PhoneMic Microphone";
    std::string sinks, sources;
    if (!run("pactl list short sinks", &sinks)) {
        vm.help =
            "PulseAudio/PipeWire (pactl) was not found, so the virtual microphone couldn't be "
            "created. Install pactl (package pulseaudio-utils or pipewire-pulse) and restart PhoneMic.";
        return vm;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    std::string idx;
    if (!has_column(sinks, "PhoneMic")) {
        if (!run("pactl load-module module-null-sink sink_name=PhoneMic "
                 "sink_properties=device.description=PhoneMic",
                 &idx)) {
            vm.help = "pactl could not create the PhoneMic sink (is the sound server running?).";
            return vm;
        }
        g_loaded_modules.push_back(trimmed(idx));
    }
    run("pactl list short sources", &sources);
    if (!has_column(sources, "PhoneMic_Mic")) {
        // The description is quoted for the proplist parser so it keeps its space
        // (the old build registered "PhoneMicMicrophone" but told users to pick
        // "PhoneMic Microphone").
        if (run("pactl load-module module-remap-source master=PhoneMic.monitor "
                "source_name=PhoneMic_Mic "
                "'source_properties=device.description=\"PhoneMic Microphone\" "
                "device.icon_name=audio-input-microphone'",
                &idx))
            g_loaded_modules.push_back(trimmed(idx));
    }
    // ALSA-backend fallback: the pulse ALSA plugin honours PULSE_SINK.
    ::setenv("PULSE_SINK", "PhoneMic", 1);
    vm.ready = true;
    if (log) log->add("Virtual microphone ready: \"PhoneMic Microphone\"");
    return vm;
}

void teardown_virtual_mic() {
    std::lock_guard<std::mutex> lk(g_mu);
    // Unload in reverse (the remapped source depends on the sink's monitor).
    for (auto it = g_loaded_modules.rbegin(); it != g_loaded_modules.rend(); ++it) {
        bool numeric = !it->empty();
        for (char c : *it) numeric = numeric && c >= '0' && c <= '9';
        if (numeric) run("pactl unload-module " + *it, nullptr);
    }
    g_loaded_modules.clear();
}

#elif defined(_WIN32)

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    vm.playback_match = "CABLE Input";
    vm.mic_name = "CABLE Output (VB-Audio Virtual Cable)";
    for (const auto& d : AudioSystem::get().playback_devices()) {
        if (icontains(d.name, "CABLE Input")) {
            vm.ready = true;
            break;
        }
    }
    if (!vm.ready)
        vm.help = "Install the free VB-CABLE driver (button below, then reboot) so apps can use "
                  "your phone as a microphone.";
    else if (log)
        log->add("Virtual microphone ready: VB-CABLE");
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

#elif defined(__APPLE__)

VirtualMic setup_virtual_mic(EventLog* log) {
    VirtualMic vm;
    vm.playback_match = "BlackHole";
    for (const auto& d : AudioSystem::get().playback_devices()) {
        if (icontains(d.name, "BlackHole")) {
            vm.ready = true;
            vm.mic_name = d.name;
            break;
        }
    }
    if (!vm.ready)
        vm.help = "Install the free BlackHole driver (brew install blackhole-2ch), then restart "
                  "PhoneMic and pick \"BlackHole 2ch\" as the microphone in your apps.";
    else if (log)
        log->add("Virtual microphone ready: " + vm.mic_name);
    return vm;
}

void teardown_virtual_mic() {}

#else

VirtualMic setup_virtual_mic(EventLog*) {
    VirtualMic vm;
    vm.help = "No virtual microphone support on this platform.";
    return vm;
}
void teardown_virtual_mic() {}

#endif

}  // namespace phonemic
