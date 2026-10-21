// Desktop app state: owns the engine pieces and the user's settings. The UI
// (ui.cpp) reads this and calls the action methods; nothing here touches ImGui.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "phonemic/audio_out.hpp"
#include "phonemic/bluetooth.hpp"
#include "phonemic/net.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/settings.hpp"
#include "phonemic/usb.hpp"
#include "phonemic/util.hpp"
#include "phonemic/virtual_mic.hpp"
#include "phonemic/wifi.hpp"

namespace phonemic {

class App {
public:
    App();
    ~App();

    // Called once per UI frame.
    void tick();

    // ---- actions ----
    void set_voice_focus(bool on);
    void set_effect(int e);
    void set_gain_db(float db);
    void set_mute(bool m);
    void set_monitor(bool on);
    void select_mic_device(const std::string& name);      // "" = automatic
    void select_monitor_device(const std::string& name);  // "" = system default
    void set_latency_ms(int ms);
    void set_theme(int t);
    void set_accent(int a);
    void refresh_devices();
    // ---- connections ----
    void set_wifi_enabled(bool on);
    void set_port(uint16_t port);         // Wi-Fi and USB restart on the new port
    void apply_passphrase(const std::string& p);
    void set_remember_passphrase(bool on);
    void set_usb_enabled(bool on);
    void refresh_addresses();
#ifdef _WIN32
    // Download and install VB-CABLE (signed by VB-Audio), then switch to it.
    void install_vbcable();
    // Name the cable's endpoints "PhoneMic Microphone"/"PhoneMic Feed", or back.
    void rename_cable(bool to_phonemic);
#endif
    // Look for the virtual microphone again (after installing VB-CABLE).
    void redetect_virtual_mic();
    // Make PhoneMic the system's default microphone (apps without a picker).
    void make_default_mic();
#if defined(__linux__)
    // Keep the mic registered when PhoneMic is closed.
    void set_persistent_mic(bool on);
#endif

    // ---- state the UI reads ----
    Settings settings;
    EventLog log{80};
    VirtualMic vmic;
    std::vector<AudioDeviceInfo> devices;
    std::string mic_error, monitor_error;
    bool mic_is_virtual = false;  // routing into the virtual microphone
    DefaultMicState default_mic = DefaultMicState::Unknown;  // refreshed every few seconds
    Receiver::Snapshot snap;
    AdaptivePlayout::Stats mic_stats;
    std::vector<net::LocalAddress> addresses;
    std::string wifi_error, usb_error;

    std::string setup_message() const;
    bool setup_busy() const;

    Receiver& receiver() { return *rx_; }
    const BluetoothServer& bluetooth() const { return *bt_; }
    const WifiServer& wifi() const { return *wifi_; }
    const UsbServer& usb() const { return *usb_; }
    const AdbBridge& adb() const { return adb_; }

private:
    void open_mic_output();
    void open_monitor_output();
    void start_wifi();
    void start_usb();
    void mark_dirty() { dirty_at_ = mono_ms(); }

    std::unique_ptr<Receiver> rx_;
    std::unique_ptr<BluetoothServer> bt_;
    std::unique_ptr<WifiServer> wifi_;
    std::unique_ptr<UsbServer> usb_;
    AdbBridge adb_{&log};
    AudioOutput mic_out_, mon_out_;
    uint64_t dirty_at_ = 0, last_default_check_ = 0, last_addr_refresh_ = 0;
    // Shared with the installer's background thread, which may outlive App.
    struct SetupState {
        std::mutex mu;
        std::string msg;
        bool busy = false;
        bool redetect = false;  // an install/rename finished: look for the mic again
    };
    std::shared_ptr<SetupState> setup_ = std::make_shared<SetupState>();
};

}  // namespace phonemic
