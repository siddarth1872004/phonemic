#include "app.hpp"

#include <algorithm>

namespace phonemic {

App::App() {
    settings = Settings::load(Settings::default_file());
    if (settings.web_token.empty()) {
        settings.web_token = random_token(8);
        mark_dirty();
    }
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
    rx_->set_mic_sink(&mic_out_);
    rx_->set_monitor_sink(&mon_out_);
    restart_receiver();

    usb_ = std::make_unique<UsbIngest>(rx_.get(), &log);
    if (settings.usb_enabled) set_usb_enabled(true);

#ifdef PHONEMIC_HAVE_WEB
    web_ = std::make_unique<WebServer>(rx_.get(), &log);
    if (settings.web_enabled) set_web_enabled(true);
#endif
}

App::~App() {
    adb_.stop();
    if (usb_) usb_->stop();
#ifdef PHONEMIC_HAVE_WEB
    if (web_) web_->stop();
#endif
    rx_->stop();
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
    const uint64_t now = mono_ms();
    if (dirty_at_ && now - dirty_at_ > 800) {
        settings.save(Settings::default_file());
        dirty_at_ = 0;
    }
    // Wi-Fi changes / VPNs come and go: keep the shown address current.
    if (now - last_addr_refresh_ > 5000) refresh_addresses();
}

void App::restart_receiver() {
    bind_error.clear();
    std::string err;
    if (!rx_->start(settings.port, &err)) {
        bind_error = err;
        log.add("Could not start: " + err);
    }
}

void App::open_mic_output() {
    mic_error.clear();
    mic_out_.set_latency_ms(static_cast<uint32_t>(settings.latency_ms));
    // Automatic = the virtual cable/sink if present. The old app on Linux opened
    // the default device and then *never* fed the virtual mic (silent mic bug).
    const std::string want = !settings.mic_device.empty() ? settings.mic_device
                             : vmic.ready                  ? vmic.playback_match
                                                           : std::string();
    std::string err;
    bool ok = mic_out_.open(want, &err);
    if (!ok && !want.empty()) ok = mic_out_.open("", &err);
    if (!ok) mic_error = err;
    mic_is_virtual = ok && vmic.ready && icontains(mic_out_.device_name(), vmic.playback_match);
}

void App::open_monitor_output() {
    monitor_error.clear();
    mon_out_.close();
    if (!settings.monitor) return;
    mon_out_.set_latency_ms(static_cast<uint32_t>(settings.latency_ms));
    std::string err;
    if (!mon_out_.open(settings.monitor_device, &err)) monitor_error = err;
}

void App::apply_passphrase(const std::string& p) {
    settings.passphrase = p;
    rx_->set_passphrase(p);
    log.add(p.empty() ? "PIN removed: unencrypted phones accepted" : "PIN set: only encrypted audio accepted");
    mark_dirty();
}

void App::set_remember_passphrase(bool on) {
    settings.remember_passphrase = on;
    mark_dirty();
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

void App::refresh_devices() { devices = AudioSystem::get().playback_devices(); }

void App::refresh_addresses() {
    addresses = net::local_addresses();
    last_addr_refresh_ = mono_ms();
}

void App::set_web_enabled(bool on) {
    settings.web_enabled = on;
    mark_dirty();
#ifdef PHONEMIC_HAVE_WEB
    web_error.clear();
    if (!on) {
        web_->stop();
        return;
    }
    std::string err;
    if (!web_->start(settings.web_port, settings.web_token, &err)) web_error = err;
#endif
}

void App::set_usb_enabled(bool on) {
    settings.usb_enabled = on;
    mark_dirty();
    if (!on) {
        adb_.stop();
        usb_->stop();
        return;
    }
    std::string err;
    if (!usb_->running() && !usb_->start(settings.port, &err)) log.add("USB: " + err);
    adb_.start(settings.port);
}

void App::new_web_token() {
    settings.web_token = random_token(8);
#ifdef PHONEMIC_HAVE_WEB
    web_->set_token(settings.web_token);
#endif
    log.add("New browser pairing code created (old links stop working)");
    mark_dirty();
}

bool App::web_running() const {
#ifdef PHONEMIC_HAVE_WEB
    return web_ && web_->running();
#else
    return false;
#endif
}

std::string App::web_url() const {
#ifdef PHONEMIC_HAVE_WEB
    const std::string ip = addresses.empty() ? "<this-pc-ip>" : addresses[0].ip;
    return WebServer::url_for(ip, settings.web_port, settings.web_token);
#else
    return {};
#endif
}

std::string App::setup_message() const {
    std::lock_guard<std::mutex> lk(setup_->mu);
    return setup_->msg;
}

bool App::setup_busy() const {
    std::lock_guard<std::mutex> lk(setup_->mu);
    return setup_->busy;
}

#ifdef _WIN32
void App::install_vbcable() {
    {
        std::lock_guard<std::mutex> lk(setup_->mu);
        if (setup_->busy) return;
        setup_->busy = true;
        setup_->msg = "Downloading VB-CABLE...";
    }
    install_vbcable_async([st = setup_](bool, std::string msg) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->busy = false;
        st->msg = std::move(msg);
    });
}
#endif

}  // namespace phonemic
