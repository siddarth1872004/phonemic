// libdbus, loaded at run time (like libpulse in pulse_ctl.cpp), so neither
// headers nor a hard dependency are needed. Used by the BlueZ service
// (bluetooth_linux.cpp) and the desktop app's tray icon (gui/tray_linux.cpp).
#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace phonemic::dbus {

// The libdbus entry points we use, declared here so no headers are needed.
// Signatures, struct layouts and constants are libdbus's stable public ABI
// (dbus/dbus-errors.h, dbus-message.h, dbus-connection.h, dbus-protocol.h).
struct DBusConnection;
struct DBusMessage;
struct DBusError {
    const char* name;
    const char* message;
    unsigned int dummy_bits;
    void* padding1;
};
struct DBusMessageIter {  // opaque to callers; larger than libdbus's 72 bytes
    alignas(void*) unsigned char opaque[128];
};
using dbus_bool_t = uint32_t;
using MessageFn = int (*)(DBusConnection*, DBusMessage*, void*);
struct DBusObjectPathVTable {
    void (*unregister_function)(DBusConnection*, void*);
    MessageFn message_function;
    void (*pad[4])(void*);
};

constexpr int DBUS_BUS_SESSION = 0, DBUS_BUS_SYSTEM = 1;
constexpr int DBUS_HANDLER_RESULT_HANDLED = 0, DBUS_HANDLER_RESULT_NOT_YET_HANDLED = 1;
constexpr unsigned DBUS_NAME_FLAG_DO_NOT_QUEUE = 4;
constexpr int DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER = 1;
constexpr int T_INVALID = 0, T_BYTE = 'y', T_STRING = 's', T_PATH = 'o', T_BOOL = 'b', T_I32 = 'i', T_U32 = 'u',
              T_U16 = 'q', T_FD = 'h', T_ARRAY = 'a', T_VARIANT = 'v', T_STRUCT = 'r', T_DICT_ENTRY = 'e';
constexpr int kCallTimeoutMs = 3000;

struct Api {
    void* lib = nullptr;
    void (*error_init)(DBusError*);
    void (*error_free)(DBusError*);
    dbus_bool_t (*error_is_set)(const DBusError*);
    DBusConnection* (*bus_get_private)(int, DBusError*);
    int (*request_name)(DBusConnection*, const char*, unsigned, DBusError*);
    void (*set_exit_on_disconnect)(DBusConnection*, dbus_bool_t);
    void (*connection_close)(DBusConnection*);
    void (*connection_unref)(DBusConnection*);
    dbus_bool_t (*read_write_dispatch)(DBusConnection*, int);
    dbus_bool_t (*register_path)(DBusConnection*, const char*, const DBusObjectPathVTable*, void*, DBusError*);
    dbus_bool_t (*unregister_path)(DBusConnection*, const char*);
    DBusMessage* (*send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
    dbus_bool_t (*send)(DBusConnection*, DBusMessage*, uint32_t*);
    void (*flush)(DBusConnection*);
    DBusMessage* (*new_method_call)(const char*, const char*, const char*, const char*);
    DBusMessage* (*new_method_return)(DBusMessage*);
    DBusMessage* (*new_error)(DBusMessage*, const char*, const char*);
    void (*message_unref)(DBusMessage*);
    dbus_bool_t (*is_method_call)(DBusMessage*, const char*, const char*);
    dbus_bool_t (*get_args)(DBusMessage*, DBusError*, int, ...);
    dbus_bool_t (*iter_init)(DBusMessage*, DBusMessageIter*);
    void (*iter_init_append)(DBusMessage*, DBusMessageIter*);
    dbus_bool_t (*append_basic)(DBusMessageIter*, int, const void*);
    dbus_bool_t (*open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
    dbus_bool_t (*close_container)(DBusMessageIter*, DBusMessageIter*);
    int (*arg_type)(DBusMessageIter*);
    void (*recurse)(DBusMessageIter*, DBusMessageIter*);
    void (*get_basic)(DBusMessageIter*, void*);
    dbus_bool_t (*next)(DBusMessageIter*);

    bool load() {
        lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn != nullptr;
        };
        sym(error_init, "dbus_error_init");
        sym(error_free, "dbus_error_free");
        sym(error_is_set, "dbus_error_is_set");
        sym(bus_get_private, "dbus_bus_get_private");
        sym(request_name, "dbus_bus_request_name");
        sym(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
        sym(connection_close, "dbus_connection_close");
        sym(connection_unref, "dbus_connection_unref");
        sym(read_write_dispatch, "dbus_connection_read_write_dispatch");
        sym(register_path, "dbus_connection_try_register_object_path");
        sym(unregister_path, "dbus_connection_unregister_object_path");
        sym(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
        sym(send, "dbus_connection_send");
        sym(flush, "dbus_connection_flush");
        sym(new_method_call, "dbus_message_new_method_call");
        sym(new_method_return, "dbus_message_new_method_return");
        sym(new_error, "dbus_message_new_error");
        sym(message_unref, "dbus_message_unref");
        sym(is_method_call, "dbus_message_is_method_call");
        sym(get_args, "dbus_message_get_args");
        sym(iter_init, "dbus_message_iter_init");
        sym(iter_init_append, "dbus_message_iter_init_append");
        sym(append_basic, "dbus_message_iter_append_basic");
        sym(open_container, "dbus_message_iter_open_container");
        sym(close_container, "dbus_message_iter_close_container");
        sym(arg_type, "dbus_message_iter_get_arg_type");
        sym(recurse, "dbus_message_iter_recurse");
        sym(get_basic, "dbus_message_iter_get_basic");
        sym(next, "dbus_message_iter_next");
        if (!ok) {
            dlclose(lib);
            lib = nullptr;
        }
        return ok;
    }
};

// Call a method that returns a message; empty `err` on success. Caller unrefs.
inline DBusMessage* call(Api& api, DBusConnection* c, DBusMessage* m, std::string* err,
                         std::string* err_name = nullptr) {
    DBusError e;
    api.error_init(&e);
    DBusMessage* r = api.send_with_reply_and_block(c, m, kCallTimeoutMs, &e);
    api.message_unref(m);
    if (api.error_is_set(&e)) {
        if (err) *err = e.message ? e.message : "D-Bus call failed";
        if (err_name) *err_name = e.name ? e.name : "";
        api.error_free(&e);
        return nullptr;
    }
    return r;
}

// Send a reply (or error) message and drop it.
inline void send_reply(Api& api, DBusConnection* c, DBusMessage* r) {
    if (!r) return;
    api.send(c, r, nullptr);
    api.flush(c);
    api.message_unref(r);
}

inline void reply_ok(Api& api, DBusConnection* c, DBusMessage* m) { send_reply(api, c, api.new_method_return(m)); }

}  // namespace phonemic::dbus
