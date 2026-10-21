#include "app.hpp"

#include <algorithm>

namespace phonemic {

App::App() {
    settings = Settings::load(Settings::default_file());
    vmic = setup_virtual_mic(&log);
    refresh_devices();
    refresh_addresses();

    rx_ = std::make_unique<Receiver>(&log);
    rx_->set_passphrase(settings.passphrase);
    rx_->set_voice_focus(settings.voice_focus);
    rx_->set_effect(effect_from_u8(static_cast<uint8_t>(settings.effect)));
    rx_->set_gain_db(settings.gain_db);
    rx_->set_mute(settings.mute);
    rx_->set_monitor(settings.monitor);

    open_mic_output();
    open_monitor_output();
    rx_->set_monitor_sink(&mon_out_);

    bt_ = std::make_unique<BluetoothServer>(rx_.get(), &log);
    bt_->start();
    wifi_ = std::make_unique<WifiServer>(rx_.get(), &log);
    if (settings.wifi_enabled) start_wifi();
    usb_ = std::make_unique<UsbServer>(rx_.get(), &log);
    if (settings.usb_enabled) start_usb();
}

App::~App() {
    adb_.stop();
    usb_->stop();
    wifi_->stop();
    bt_->stop();
    rx_->set_mic_sink(nullptr);
    rx_->set_monitor_sink(nullptr);
    mic_out_.close();
    mon_out_.close();
    teardown_virtual_mic();
    settings.save(Settings::default_file());
}

void App::tick() {
    snap = rx_->snapshot();
    mic_stats = mic_out_.stats();
    bool redetect = false;
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        std::swap(redetect, setup_->redetect);
    }
    if (redetect) redetect_virtual_mic();
    const uint64_t now = mono_ms();
    if (dirty_at_ && now - dirty_at_ > 800) {
        settings.save(Settings::default_file());
        dirty_at_ = 0;
    }
    if (now - last_addr_refresh_ > 5000) refresh_addresses();
    // The user may change the default microphone in the system settings.
    if (vmic.ready && now - last_default_check_ > 3000) {
        default_mic = default_mic_state(vmic);
        last_default_check_ = now;
    }
}

void App::open_mic_output() {
    mic_error.clear();
    // Detach the receiver first: it must never write into a sink being replaced.
    rx_->set_mic_sink(nullptr);
    mic_out_.close();
    mic_out_.set_latency_ms(static_cast<uint32_t>(settings.latency_ms));
    // Automatic = the virtual mic's playback side if there is one. The old app on
    // Linux opened the default device and then *never* fed the virtual mic.
    const std::string want = !settings.mic_device.empty()                    ? settings.mic_device
                             : vmic.ready && !vmic.playback_match.empty() ? vmic.playback_match
                                                                             : std::string();
    std::string err;
    bool ok = mic_out_.open(want, &err);
    if (!ok && !want.empty()) ok = mic_out_.open("", &err);
    if (!ok) mic_error = err;
    mic_is_virtual = ok && vmic.ready && !vmic.playback_match.empty() &&
                     icontains(mic_out_.device_name(), vmic.playback_match);
    rx_->set_mic_sink(&mic_out_);
}

void App::redetect_virtual_mic() {
    rx_->set_mic_sink(nullptr);
    mic_out_.close();
    teardown_virtual_mic();
    vmic = setup_virtual_mic(&log);
    refresh_devices();
    open_mic_output();
}

void App::open_monitor_output() {
    monitor_error.clear();
    mon_out_.close();
    if (!settings.monitor) return;
    mon_out_.set_latency_ms(static_cast<uint32_t>(settings.latency_ms));
    std::string err;
    if (!mon_out_.open(settings.monitor_device, &err)) monitor_error = err;
}

void App::set_voice_focus(bool on) {
    settings.voice_focus = on;
    rx_->set_voice_focus(on);
    mark_dirty();
}

void App::set_effect(int e) {
    settings.effect = e;
    rx_->set_effect(effect_from_u8(static_cast<uint8_t>(e)));
    mark_dirty();
}

void App::set_gain_db(float db) {
    settings.gain_db = db;
    rx_->set_gain_db(db);
    mark_dirty();
}

void App::set_mute(bool m) {
    settings.mute = m;
    rx_->set_mute(m);
    mark_dirty();
}

void App::set_monitor(bool on) {
    settings.monitor = on;
    open_monitor_output();
    rx_->set_monitor(on);
    mark_dirty();
}

void App::select_mic_device(const std::string& name) {
    settings.mic_device = name;
    open_mic_output();
    mark_dirty();
}

void App::select_monitor_device(const std::string& name) {
    settings.monitor_device = name;
    open_monitor_output();
    mark_dirty();
}

void App::set_latency_ms(int ms) {
    settings.latency_ms = ms;
    mic_out_.set_latency_ms(static_cast<uint32_t>(ms));
    mon_out_.set_latency_ms(static_cast<uint32_t>(ms));
    mark_dirty();
}

void App::set_theme(int t) {
    settings.theme = t;
    mark_dirty();
}

void App::set_accent(int a) {
    settings.accent = a;
    mark_dirty();
}

void App::refresh_devices() { devices = AudioSystem::get().playback_devices(); }

void App::refresh_addresses() {
    addresses = net::local_addresses();
    last_addr_refresh_ = mono_ms();
}

void App::start_wifi() {
    wifi_error.clear();
    std::string err;
    if (!wifi_->start(settings.port, &err)) {
        wifi_error = err;
        log.add("Wi-Fi: " + err);
    }
}

void App::start_usb() {
    usb_error.clear();
    std::string err;
    if (!usb_->start(settings.port, &err)) {
        usb_error = err;
        log.add("USB: " + err);
        return;
    }
    adb_.start(usb_->port());
}

void App::set_wifi_enabled(bool on) {
    settings.wifi_enabled = on;
    mark_dirty();
    wifi_->stop();
    wifi_error.clear();
    if (on) start_wifi();
}

void App::set_port(uint16_t port) {
    if (port == settings.port) return;
    settings.port = port;
    mark_dirty();
    if (settings.wifi_enabled) {
        wifi_->stop();
        start_wifi();
    }
    if (settings.usb_enabled) {
        adb_.stop();
        usb_->stop();
        start_usb();
    }
}

void App::apply_passphrase(const std::string& p) {
    if (p == settings.passphrase) return;
    settings.passphrase = p;
    rx_->set_passphrase(p);
    log.add(p.empty() ? "PIN removed: Wi-Fi phones need no PIN" : "PIN set: Wi-Fi phones must use it");
    mark_dirty();
}

void App::set_remember_passphrase(bool on) {
    settings.remember_passphrase = on;
    mark_dirty();
}

void App::set_usb_enabled(bool on) {
    settings.usb_enabled = on;
    mark_dirty();
    adb_.stop();
    usb_->stop();
    usb_error.clear();
    if (on) start_usb();
}

std::string App::setup_message() const {
    std::lock_guard<std::mutex> lk(setup_->mu);
    return setup_->msg;
}

bool App::setup_busy() const {
    std::lock_guard<std::mutex> lk(setup_->mu);
    return setup_->busy;
}

void App::make_default_mic() {
    std::string err;
    const bool ok = phonemic::make_default_mic(vmic, &err);
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        setup_->msg = ok ? "PhoneMic is now the default microphone." : "Setup failed: couldn't make it the default (" + err + ").";
    }
    default_mic = default_mic_state(vmic);
    last_default_check_ = mono_ms();
    log.add(ok ? "PhoneMic Microphone set as the default microphone" : "Default microphone not changed: " + err);
}

#if defined(__linux__)
void App::set_persistent_mic(bool on) {
    std::string err;
    const bool ok = phonemic::set_persistent_mic(on, vmic.via == "PipeWire", &err);
    vmic.persistent = persistent_mic_installed();
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        setup_->msg = !ok ? "Setup failed: " + err
                      : on ? "PhoneMic Microphone stays available when PhoneMic is closed (from your next login on)."
                           : "PhoneMic Microphone now goes away when PhoneMic closes.";
    }
    log.add(ok ? (on ? "Microphone kept registered when PhoneMic is closed" : "Microphone no longer kept registered")
               : "Couldn't change: " + err);
}
#endif

#ifdef _WIN32
void App::install_vbcable() {
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        if (setup_->busy) return;
        setup_->busy = true;
        setup_->msg = "Installing VB-CABLE (approve the Windows prompt)...";
    }
    install_vbcable_async([st = setup_](bool, std::string msg) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->busy = false;
        st->msg = std::move(msg);
        st->redetect = true;
    });
}

void App::rename_cable(bool to_phonemic) {
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        if (setup_->busy) return;
        setup_->busy = true;
        setup_->msg = "Renaming the cable (approve the Windows prompt)...";
    }
    rename_cable_async(to_phonemic, [st = setup_](bool, std::string msg) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->busy = false;
        st->msg = std::move(msg);
        st->redetect = true;
    });
}
#endif
}  // namespace phonemic
