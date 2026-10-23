// Linux: publish the PhoneMic RFCOMM service through BlueZ.
//
// BlueZ's ProfileManager1.RegisterProfile writes the SDP record, listens on the
// RFCOMM channel and hands every paired phone that connects to our Profile1
// object as a ready socket (NewConnection). libdbus is loaded at run time
// (dbus_dl.hpp), so neither headers nor a hard dependency are needed.
#include "phonemic/bluetooth.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <phonemic/wire.h>

#include <cstring>

#include "dbus_dl.hpp"

namespace phonemic {

namespace {

using namespace dbus;

constexpr int kAfBluetooth = 31, kBtProtoRfcomm = 3;  // linux/bluetooth.h
struct sockaddr_rc {
    sa_family_t rc_family;
    uint8_t rc_bdaddr[6];
    uint8_t rc_channel;
};

const char* const kProfilePath = "/org/phonemic/profile";

struct Adapter {
    bool found = false;
    bool powered = false;
    std::string name;
};

struct Ctx {
    Api* api;
    BluetoothServer* server;
};

// Walk GetManagedObjects (a{oa{sa{sv}}}) for the first adapter, preferring a
// powered one.
Adapter find_adapter(Api& api, DBusConnection* c, std::string* err, std::string* err_name) {
    Adapter best;
    DBusMessage* m = api.new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage* r = m ? call(api, c, m, err, err_name) : nullptr;
    if (!r) return best;
    DBusMessageIter top, objs;
    if (api.iter_init(r, &top) && api.arg_type(&top) == T_ARRAY) {
        api.recurse(&top, &objs);
        for (; api.arg_type(&objs) == T_DICT_ENTRY; api.next(&objs)) {
            DBusMessageIter obj, ifaces;
            api.recurse(&objs, &obj);
            if (!api.next(&obj) || api.arg_type(&obj) != T_ARRAY) continue;  // skip the object path
            api.recurse(&obj, &ifaces);
            for (; api.arg_type(&ifaces) == T_DICT_ENTRY; api.next(&ifaces)) {
                DBusMessageIter iface, props;
                api.recurse(&ifaces, &iface);
                const char* iname = nullptr;
                api.get_basic(&iface, &iname);
                if (!iname || std::strcmp(iname, "org.bluez.Adapter1") != 0) continue;
                if (!api.next(&iface) || api.arg_type(&iface) != T_ARRAY) continue;
                Adapter a;
                a.found = true;
                api.recurse(&iface, &props);
                for (; api.arg_type(&props) == T_DICT_ENTRY; api.next(&props)) {
                    DBusMessageIter prop, var;
                    api.recurse(&props, &prop);
                    const char* key = nullptr;
                    api.get_basic(&prop, &key);
                    if (!key || !api.next(&prop) || api.arg_type(&prop) != T_VARIANT) continue;
                    api.recurse(&prop, &var);
                    if (std::strcmp(key, "Alias") == 0 && api.arg_type(&var) == T_STRING) {
                        const char* v = nullptr;
                        api.get_basic(&var, &v);
                        if (v) a.name = v;
                    } else if (std::strcmp(key, "Powered") == 0 && api.arg_type(&var) == T_BOOL) {
                        dbus_bool_t v = 0;
                        api.get_basic(&var, &v);
                        a.powered = v != 0;
                    }
                }
                if (!best.found || (a.powered && !best.powered)) best = a;
            }
        }
    }
    api.message_unref(r);
    return best;
}

std::string device_alias(Api& api, DBusConnection* c, const char* device_path) {
    DBusMessage* m = api.new_method_call("org.bluez", device_path, "org.freedesktop.DBus.Properties", "Get");
    if (!m) return {};
    const char* iface = "org.bluez.Device1";
    const char* prop = "Alias";
    DBusMessageIter it;
    api.iter_init_append(m, &it);
    api.append_basic(&it, T_STRING, &iface);
    api.append_basic(&it, T_STRING, &prop);
    DBusMessage* r = call(api, c, m, nullptr);
    if (!r) return {};
    std::string name;
    DBusMessageIter top, var;
    if (api.iter_init(r, &top) && api.arg_type(&top) == T_VARIANT) {
        api.recurse(&top, &var);
        const char* v = nullptr;
        if (api.arg_type(&var) == T_STRING) api.get_basic(&var, &v);
        if (v) name = v;
    }
    api.message_unref(r);
    return name;
}

// org.bluez.Profile1, called by bluetoothd.
int on_profile_message(DBusConnection* c, DBusMessage* m, void* user) {
    auto* ctx = static_cast<Ctx*>(user);
    Api& api = *ctx->api;
    if (api.is_method_call(m, "org.bluez.Profile1", "NewConnection")) {
        DBusError e;
        api.error_init(&e);
        const char* device = nullptr;
        int fd = -1;
        if (!api.get_args(m, &e, T_PATH, &device, T_FD, &fd, T_INVALID)) {
            api.error_free(&e);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        // BlueZ may hand the socket over non-blocking; writes below expect to block
        // (bounded by a send timeout so a stalled phone can't wedge a thread).
        const int fl = fcntl(fd, F_GETFL);
        if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        const std::string name = device_alias(api, c, device);
        reply_ok(api, c, m);
        ctx->server->accept(socket_stream(fd), name);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, "org.bluez.Profile1", "RequestDisconnection") ||
        api.is_method_call(m, "org.bluez.Profile1", "Release")) {
        reply_ok(api, c, m);  // the phone's stream ends when its socket closes
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// A free RFCOMM channel: the kernel assigns one to a listening socket bound to
// channel 0. BlueZ then listens on it for us.
int free_rfcomm_channel(std::string* err) {
    const int s = ::socket(kAfBluetooth, SOCK_STREAM, kBtProtoRfcomm);
    if (s < 0) {
        *err = "this system has no Bluetooth (RFCOMM) support";
        return -1;
    }
    sockaddr_rc a{};
    a.rc_family = kAfBluetooth;
    socklen_t len = sizeof a;
    int ch = -1;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(s, 1) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0 && a.rc_channel > 0)
        ch = a.rc_channel;
    ::close(s);
    if (ch < 0) *err = "no free Bluetooth RFCOMM channel";
    return ch;
}

void add_option(Api& api, DBusMessageIter* dict, const char* key, int type, const void* value) {
    const char sig[2] = {static_cast<char>(type), 0};
    DBusMessageIter entry, var;
    api.open_container(dict, T_DICT_ENTRY, nullptr, &entry);
    api.append_basic(&entry, T_STRING, &key);
    api.open_container(&entry, T_VARIANT, sig, &var);
    api.append_basic(&var, type, value);
    api.close_container(&entry, &var);
    api.close_container(dict, &entry);
}

bool register_profile(Api& api, DBusConnection* c, uint16_t channel, std::string* err) {
    DBusMessage* m = api.new_method_call("org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");
    if (!m) return false;
    const char* path = kProfilePath;
    const char* uuid = PM_SERVICE_UUID;
    const char* name = PM_SERVICE_NAME;
    const char* role = "server";
    const dbus_bool_t yes = 1, no = 0;
    DBusMessageIter it, dict;
    api.iter_init_append(m, &it);
    api.append_basic(&it, T_PATH, &path);
    api.append_basic(&it, T_STRING, &uuid);
    api.open_container(&it, T_ARRAY, "{sv}", &dict);
    add_option(api, &dict, "Name", T_STRING, &name);
    add_option(api, &dict, "Role", T_STRING, &role);
    add_option(api, &dict, "Channel", T_U16, &channel);
    add_option(api, &dict, "RequireAuthentication", T_BOOL, &yes);  // paired phones only
    add_option(api, &dict, "RequireAuthorization", T_BOOL, &no);    // no prompt per connection
    add_option(api, &dict, "AutoConnect", T_BOOL, &no);
    api.close_container(&it, &dict);
    DBusMessage* r = call(api, c, m, err);
    if (!r) return false;
    api.message_unref(r);
    return true;
}

void unregister_profile(Api& api, DBusConnection* c) {
    DBusMessage* m = api.new_method_call("org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
    if (!m) return;
    const char* path = kProfilePath;
    DBusMessageIter it;
    api.iter_init_append(m, &it);
    api.append_basic(&it, T_PATH, &path);
    if (DBusMessage* r = call(api, c, m, nullptr)) api.message_unref(r);
}

}  // namespace

void BluetoothServer::platform_loop() {
    static Api api;
    static const bool loaded = api.load();
    if (!loaded) {
        set_state(State::Unavailable, "libdbus-1 is not installed");
        return;
    }
    Ctx ctx{&api, this};
    const DBusObjectPathVTable vtable{nullptr, on_profile_message, {}};

    while (!stop_.load()) {
        DBusError e;
        api.error_init(&e);
        DBusConnection* c = api.bus_get_private(DBUS_BUS_SYSTEM, &e);
        if (!c) {
            api.error_free(&e);
            set_state(State::Unavailable, "can't reach the system D-Bus");
            if (!sleep_unless_stopped(5000)) break;
            continue;
        }
        api.set_exit_on_disconnect(c, 0);

        bool registered = false;
        uint64_t last_check = 0;
        while (!stop_.load()) {
            const uint64_t now = mono_ms();
            if (now - last_check >= 3000) {
                last_check = now;
                std::string err, err_name;
                const Adapter a = find_adapter(api, c, &err, &err_name);
                if (!a.found) {
                    set_state(State::Unavailable, err_name.find("ServiceUnknown") != std::string::npos ||
                                                          err_name.find("NameHasNoOwner") != std::string::npos
                                                      ? "the Bluetooth service (bluetoothd) is not running"
                                                      : "no Bluetooth adapter found");
                    if (registered) break;  // bluetoothd went away: start over with it
                } else {
                    set_adapter_name(a.name);
                    if (!registered) {
                        const int ch = free_rfcomm_channel(&err);
                        if (ch > 0 && !api.register_path(c, kProfilePath, &vtable, &ctx, &e)) {
                            err = e.message ? e.message : "can't export the D-Bus profile object";
                            api.error_free(&e);
                        } else if (ch > 0 && !register_profile(api, c, static_cast<uint16_t>(ch), &err)) {
                            api.unregister_path(c, kProfilePath);
                        } else if (ch > 0) {
                            registered = true;
                        }
                        if (!registered) set_state(State::Failed, "can't publish the PhoneMic service: " + err);
                    }
                    if (registered) set_state(a.powered ? State::Ready : State::PoweredOff);
                }
            }
            if (!api.read_write_dispatch(c, 200)) break;  // the bus connection dropped
        }
        if (registered) {
            unregister_profile(api, c);
            api.unregister_path(c, kProfilePath);
        }
        api.connection_close(c);
        api.connection_unref(c);
        if (!stop_.load() && !sleep_unless_stopped(2000)) break;
    }
}

}  // namespace phonemic
