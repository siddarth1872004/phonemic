// Making the PhoneMic microphone the system's default recording device, so apps
// without a microphone picker (and "Default" in apps that have one) use it.
// On Windows this file also looks after the VB-CABLE endpoints: finding them,
// naming them after PhoneMic, and keeping the cable off the default speaker.
#include "phonemic/virtual_mic.hpp"

#if defined(__linux__)
#include <chrono>
#include <thread>

#include "phonemic/pulse_ctl.hpp"
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <string>
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

// The free VB-CABLE's adapter name. Endpoint friendly names are
// "<endpoint name> (<adapter name>)", so this survives renaming the endpoints.
constexpr wchar_t kCableAdapter[] = L"VB-Audio Virtual Cable";

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::wstring string_prop(IPropertyStore* props, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}

std::wstring device_id(IMMDevice* dev) {
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id))) return {};
    std::wstring out(id);
    CoTaskMemFree(id);
    return out;
}

bool is_cable(IMMDevice* dev) {
    Ptr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props.p))) return false;
    return string_prop(props.p, PKEY_Device_FriendlyName).find(kCableAdapter) != std::wstring::npos;
}

// The active VB-CABLE endpoint for `flow` (nullptr if none). Caller releases.
IMMDevice* cable_endpoint(IMMDeviceEnumerator* en, EDataFlow flow) {
    Ptr<IMMDeviceCollection> all;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all.p)) || FAILED(all->GetCount(&n)))
        return nullptr;
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(all->Item(i, &dev))) continue;
        if (is_cable(dev)) return dev;
        dev->Release();
    }
    return nullptr;
}

// Endpoint id of the cable's recording side, the PhoneMic microphone ("" if none).
std::wstring phonemic_endpoint(IMMDeviceEnumerator* en) {
    Ptr<IMMDevice> dev;
    dev.p = cable_endpoint(en, eCapture);
    return dev.p ? device_id(dev.p) : std::wstring();
}

bool make_enumerator(Ptr<IMMDeviceEnumerator>* en) {
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en->p)));
}
}  // namespace

bool find_cable(CableEndpoints* out) {
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!make_enumerator(&en)) return false;
    Ptr<IMMDevice> feed, mic;
    feed.p = cable_endpoint(en.p, eRender);
    mic.p = cable_endpoint(en.p, eCapture);
    if (!feed.p || !mic.p) return false;
    Ptr<IPropertyStore> fp, mp;
    if (FAILED(feed->OpenPropertyStore(STGM_READ, &fp.p)) || FAILED(mic->OpenPropertyStore(STGM_READ, &mp.p)))
        return false;
    out->feed_name = narrow(string_prop(fp.p, PKEY_Device_FriendlyName).c_str());
    out->mic_name = narrow(string_prop(mp.p, PKEY_Device_FriendlyName).c_str());
    out->named_phonemic = string_prop(mp.p, PKEY_Device_DeviceDesc) == L"PhoneMic Microphone";
    Ptr<IMMDevice> def;
    out->default_output = SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def.p)) && is_cable(def.p);
    return true;
}

bool fix_cable_default_output(std::string* now_default) {
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!make_enumerator(&en)) return false;
    // A real speaker or headset to hand the default back to: the first active
    // playback endpoint that isn't a VB-Audio cable.
    Ptr<IMMDeviceCollection> all;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all.p)) || FAILED(all->GetCount(&n)))
        return false;
    std::wstring target, target_name;
    for (UINT i = 0; i < n && target.empty(); ++i) {
        Ptr<IMMDevice> dev;
        Ptr<IPropertyStore> props;
        if (FAILED(all->Item(i, &dev.p)) || FAILED(dev->OpenPropertyStore(STGM_READ, &props.p))) continue;
        const std::wstring name = string_prop(props.p, PKEY_Device_FriendlyName);
        if (name.find(L"VB-Audio") != std::wstring::npos) continue;
        target = device_id(dev.p);
        target_name = name;
    }
    if (target.empty()) return false;
    Ptr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                                reinterpret_cast<void**>(&policy.p))))
        return false;
    bool changed = false;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
        Ptr<IMMDevice> def;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, role, &def.p)) || !is_cable(def.p)) continue;
        if (FAILED(policy->SetDefaultEndpoint(target.c_str(), role))) return false;
        changed = true;
    }
    if (changed && now_default) *now_default = narrow(target_name.c_str());
    return changed;
}

int rename_cable_now(bool to_phonemic) {
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!make_enumerator(&en)) return 2;
    const struct {
        EDataFlow flow;
        const wchar_t* ours;
        const wchar_t* stock;
    } sides[] = {{eCapture, L"PhoneMic Microphone", L"CABLE Output"}, {eRender, L"PhoneMic Feed", L"CABLE Input"}};
    for (const auto& s : sides) {
        Ptr<IMMDevice> dev;
        dev.p = cable_endpoint(en.p, s.flow);
        if (!dev.p) return 3;
        // Writable only from an elevated process; the endpoint builder service
        // then shows the new name to every app.
        Ptr<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READWRITE, &props.p))) return 5;
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_LPWSTR;
        v.pwszVal = const_cast<wchar_t*>(to_phonemic ? s.ours : s.stock);
        if (FAILED(props->SetValue(PKEY_Device_DeviceDesc, v)) || FAILED(props->Commit())) return 5;
    }
    return 0;
}

DefaultMicState default_mic_state(const VirtualMic& vm) {
    if (!vm.ready) return DefaultMicState::Unknown;
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!make_enumerator(&en)) return DefaultMicState::Unknown;
    const std::wstring ours = phonemic_endpoint(en.p);
    if (ours.empty()) return DefaultMicState::Unknown;
    Ptr<IMMDevice> def;
    if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def.p))) return DefaultMicState::No;
    const std::wstring id = device_id(def.p);
    if (id.empty()) return DefaultMicState::Unknown;
    return ours == id ? DefaultMicState::Yes : DefaultMicState::No;
}

bool make_default_mic(const VirtualMic& vm, std::string* err) {
    ComScope com;
    Ptr<IMMDeviceEnumerator> en;
    if (!vm.ready || !make_enumerator(&en)) {
        if (err) *err = "Windows audio isn't available";
        return false;
    }
    const std::wstring ours = phonemic_endpoint(en.p);
    if (ours.empty()) {
        if (err) *err = "Windows doesn't list the VB-CABLE microphone yet";
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
