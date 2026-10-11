// Making "PhoneMic Microphone" the system's default recording device, so apps
// without a microphone picker (and "Default" in apps that have one) use it.
#include "phonemic/virtual_mic.hpp"

#if defined(__linux__)
#include <chrono>
#include <thread>

#include "phonemic/pulse_ctl.hpp"
#elif defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#endif

namespace phonemic {

#if defined(__linux__)

namespace {
// source_name of the remapped source setup_virtual_mic creates.
constexpr char kSource[] = "PhoneMic_Mic";
}  // namespace

DefaultMicState default_mic_state(const VirtualMic& vm) {
    if (!vm.ready || vm.via == "ALSA loopback") return DefaultMicState::Unknown;
    auto pulse = PulseCtl::connect(nullptr);
    if (!pulse) return DefaultMicState::Unknown;
    return pulse->default_source() == kSource ? DefaultMicState::Yes : DefaultMicState::No;
}

bool make_default_mic(const VirtualMic& vm, std::string* err) {
    if (!vm.ready || vm.via == "ALSA loopback") {
        if (err) *err = "set the default device in your ALSA configuration (~/.asoundrc)";
        return false;
    }
    std::string why;
    auto pulse = PulseCtl::connect(&why);
    if (!pulse || !pulse->set_default_source(kSource)) {
        if (err) *err = pulse ? "the sound server refused" : why;
        return false;
    }
    // PipeWire applies it asynchronously (WirePlumber picks up the configured
    // default a moment later): wait until it shows.
    for (int i = 0; i < 30; ++i) {
        if (pulse->default_source() == kSource) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (err) *err = "the sound server didn't switch to it";
    return false;
}

#elif defined(__APPLE__)

namespace {
// kAudioObjectPropertyElementMain (named so from macOS 12; we support 11).
constexpr AudioObjectPropertyElement kElementMain = 0;

// kDeviceUID in pc-app/driver-macos/PhoneMicAudio.c.
AudioObjectID phonemic_device() {
    CFStringRef uid = CFSTR("PhoneMicLoopback_UID");
    AudioObjectID dev = kAudioObjectUnknown;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                    kElementMain};
    UInt32 size = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &dev) != noErr)
        return kAudioObjectUnknown;
    return dev;
}

const AudioObjectPropertyAddress kDefaultInput = {kAudioHardwarePropertyDefaultInputDevice,
                                                  kAudioObjectPropertyScopeGlobal, kElementMain};
}  // namespace

DefaultMicState default_mic_state(const VirtualMic&) {
    const AudioObjectID ours = phonemic_device();
    if (ours == kAudioObjectUnknown) return DefaultMicState::Unknown;
    AudioObjectID current = kAudioObjectUnknown;
    UInt32 size = sizeof current;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &kDefaultInput, 0, nullptr, &size, &current) != noErr)
        return DefaultMicState::Unknown;
    return current == ours ? DefaultMicState::Yes : DefaultMicState::No;
}

bool make_default_mic(const VirtualMic&, std::string* err) {
    AudioObjectID ours = phonemic_device();
    if (ours == kAudioObjectUnknown) {
        if (err) *err = "the PhoneMic driver isn't loaded";
        return false;
    }
    if (AudioObjectSetPropertyData(kAudioObjectSystemObject, &kDefaultInput, 0, nullptr, sizeof ours, &ours) !=
        noErr) {
        if (err) *err = "Core Audio refused";
        return false;
    }
    return true;
}

#elif defined(_WIN32)

namespace {
// IPolicyConfig: the interface the Sound control panel uses to change the
// default device. Not in the SDK; this layout and these GUIDs are the ones
// Windows has shipped since Windows 7 (used by EarTrumpet, SoundSwitch, ...).
const CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const IID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR device_id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

template <typename T>
struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

// Endpoint id of the active capture endpoint named after PhoneMic ("" if none).
std::wstring phonemic_endpoint(IMMDeviceEnumerator* en) {
    Ptr<IMMDeviceCollection> all;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all.p)) || FAILED(all->GetCount(&n)))
        return {};
    for (UINT i = 0; i < n; ++i) {
        Ptr<IMMDevice> dev;
        Ptr<IPropertyStore> props;
        if (FAILED(all->Item(i, &dev.p)) || FAILED(dev->OpenPropertyStore(STGM_READ, &props.p))) continue;
        PROPVARIANT name;
        PropVariantInit(&name);
        bool ours = false;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
            ours = wcsstr(name.pwszVal, L"PhoneMic") != nullptr;
        PropVariantClear(&name);
        if (!ours) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        std::wstring out(id);
        CoTaskMemFree(id);
        return out;
    }
    return {};
}
}  // namespace

DefaultMicState default_mic_state(const VirtualMic& vm) {
    if (!vm.ready) return DefaultMicState::Unknown;
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en.p))))
        return DefaultMicState::Unknown;
    const std::wstring ours = phonemic_endpoint(en.p);
    if (ours.empty()) return DefaultMicState::Unknown;
    Ptr<IMMDevice> def;
    if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def.p))) return DefaultMicState::No;
    LPWSTR id = nullptr;
    if (FAILED(def->GetId(&id))) return DefaultMicState::Unknown;
    const bool same = ours == id;
    CoTaskMemFree(id);
    return same ? DefaultMicState::Yes : DefaultMicState::No;
}

bool make_default_mic(const VirtualMic& vm, std::string* err) {
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!vm.ready || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en.p)))) {
        if (err) *err = "Windows audio isn't available";
        return false;
    }
    const std::wstring ours = phonemic_endpoint(en.p);
    if (ours.empty()) {
        if (err) *err = "Windows doesn't list the PhoneMic microphone yet";
        return false;
    }
    Ptr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                                reinterpret_cast<void**>(&policy.p)))) {
        if (err) *err = "set it in Settings > System > Sound > Input";
        return false;
    }
    // All three roles: apps that ask for the "communications" device (Teams,
    // Skype, Discord's default) as well as everything else.
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
        if (FAILED(policy->SetDefaultEndpoint(ours.c_str(), role))) {
            if (err) *err = "Windows refused; set it in Settings > System > Sound > Input";
            return false;
        }
    }
    return true;
}

#else

DefaultMicState default_mic_state(const VirtualMic&) { return DefaultMicState::Unknown; }
bool make_default_mic(const VirtualMic&, std::string* err) {
    if (err) *err = "not supported on this platform";
    return false;
}

#endif

}  // namespace phonemic
