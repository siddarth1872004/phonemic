// Desktop app state: owns the engine pieces and the user's settings. The UI
// (ui.cpp) reads this and calls the action methods; nothing here touches ImGui.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "phonemic/audio_out.hpp"
#include "phonemic/net.hpp"
#include "phonemic/receiver.hpp"
#include "phonemic/settings.hpp"
#include "phonemic/usb.hpp"
#include "phonemic/util.hpp"
#include "phonemic/virtual_mic.hpp"
#ifdef PHONEMIC_HAVE_WEB
#include "phonemic/web_server.hpp"
#endif

namespace phonemic {

class App {
public:
    App();
    ~App();

    // Called once per UI frame.
    void tick();

    // ---- actions ----
    void restart_receiver();          // after a port change / retry
    void apply_passphrase(const std::string& p);
    void set_voice_focus(bool on);
    void set_effect(int e);
    void set_gain_db(float db);
    void set_mute(bool m);
    void set_monitor(bool on);
    void select_mic_device(const std::string& name);      // "" = automatic
    void select_monitor_device(const std::string& name);  // "" = system default
    void set_latency_ms(int ms);
    void set_web_enabled(bool on);
    void set_usb_enabled(bool on);
    AdbBridge::State usb_state() const { return adb_.state(); }
    std::string usb_device() const { return adb_.device(); }
    bool usb_listening() const { return usb_->running(); }
    void new_web_token();
    void refresh_devices();
    void refresh_addresses();
    void set_remember_passphrase(bool on);
#ifdef _WIN32
    void install_vbcable();
#endif

    // ---- state the UI reads ----
    Settings settings;
    EventLog log{80};
    VirtualMic vmic;
    std::vector<net::LocalAddress> addresses;
    std::vector<AudioDeviceInfo> devices;
    std::string bind_error, mic_error, monitor_error, web_error;
    bool mic_is_virtual = false;  // routing into the virtual cable / sink
    Receiver::Snapshot snap;
    AdaptivePlayout::Stats mic_stats;

    std::string setup_message() const;
    bool setup_busy() const;
    bool web_running() const;
    std::string web_url() const;  // for the primary address

    Receiver& receiver() { return *rx_; }

private:
    void open_mic_output();
    void open_monitor_output();
    void mark_dirty() { dirty_at_ = mono_ms(); }

    std::unique_ptr<Receiver> rx_;
    std::unique_ptr<UsbIngest> usb_;
    AdbBridge adb_{&log};
    AudioOutput mic_out_, mon_out_;
#ifdef PHONEMIC_HAVE_WEB
    std::unique_ptr<WebServer> web_;
#endif
    uint64_t dirty_at_ = 0, last_addr_refresh_ = 0;
    // Shared with the installer's background thread, which may outlive App.
    struct SetupState {
        std::mutex mu;
        std::string msg;
        bool busy = false;
    };
    std::shared_ptr<SetupState> setup_ = std::make_shared<SetupState>();
};

}  // namespace phonemic
