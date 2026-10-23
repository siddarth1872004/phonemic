// Linux tray icon: org.kde.StatusNotifierItem + com.canonical.dbusmenu on the
// session bus. See tray.hpp.
#include "tray.hpp"

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "dbus_dl.hpp"
#include "phonemic/util.hpp"

namespace phonemic {

namespace {

using namespace dbus;

const char* const kInstanceName = "io.github.siddarth1872004.phonemic";
const char* const kInstancePath = "/io/github/siddarth1872004/phonemic";
const char* const kItemPath = "/StatusNotifierItem";
const char* const kItemIface = "org.kde.StatusNotifierItem";
const char* const kMenuPath = "/MenuBar";
const char* const kMenuIface = "com.canonical.dbusmenu";
const char* const kPropsIface = "org.freedesktop.DBus.Properties";
const char* const kWatcherName = "org.kde.StatusNotifierWatcher";

// dbusmenu item ids (0 is the root).
constexpr int32_t kMenuShow = 1, kMenuQuit = 2, kMenuSeparator = 3;

}  // namespace

struct Tray::Impl {
    Api api;
    DBusConnection* c = nullptr;
    std::string item_name;
    std::vector<TrayIcon> icons;
    std::thread th;
    std::atomic<bool> stop{false}, shown{false}, toggle{false}, show{false}, quit{false};
};

namespace {

using Impl = Tray::Impl;

void put_str(Api& api, DBusMessageIter* it, const char* s) { api.append_basic(it, T_STRING, &s); }

// a(iiay): ARGB32 in network byte order, as the StatusNotifierItem spec asks.
void put_pixmaps(Api& api, DBusMessageIter* it, const std::vector<TrayIcon>& icons) {
    DBusMessageIter arr;
    api.open_container(it, T_ARRAY, "(iiay)", &arr);
    for (const TrayIcon& icon : icons) {
        DBusMessageIter st, bytes;
        api.open_container(&arr, T_STRUCT, nullptr, &st);
        const int32_t n = icon.size;
        api.append_basic(&st, T_I32, &n);
        api.append_basic(&st, T_I32, &n);
        api.open_container(&st, T_ARRAY, "y", &bytes);
        for (size_t i = 0; i + 3 < icon.rgba.size(); i += 4) {
            const uint8_t argb[4] = {icon.rgba[i + 3], icon.rgba[i], icon.rgba[i + 1], icon.rgba[i + 2]};
            for (const uint8_t b : argb) api.append_basic(&bytes, T_BYTE, &b);
        }
        api.close_container(&st, &bytes);
        api.close_container(&arr, &st);
    }
    api.close_container(it, &arr);
}

// Append one property's value as a variant; false for an unknown name.
bool put_item_prop(Impl& t, DBusMessageIter* it, const char* name) {
    Api& api = t.api;
    auto variant = [&](const char* sig, auto&& body) {
        DBusMessageIter v;
        api.open_container(it, T_VARIANT, sig, &v);
        body(&v);
        api.close_container(it, &v);
        return true;
    };
    auto str = [&](const char* s) { return variant("s", [&](DBusMessageIter* v) { put_str(api, v, s); }); };
    if (!std::strcmp(name, "Category")) return str("ApplicationStatus");
    if (!std::strcmp(name, "Id")) return str("phonemic");
    if (!std::strcmp(name, "Title")) return str("PhoneMic");
    if (!std::strcmp(name, "Status")) return str("Active");
    if (!std::strcmp(name, "IconName")) return str("phonemic");
    if (!std::strcmp(name, "IconPixmap"))
        return variant("a(iiay)", [&](DBusMessageIter* v) { put_pixmaps(api, v, t.icons); });
    if (!std::strcmp(name, "WindowId"))
        return variant("i", [&](DBusMessageIter* v) {
            const int32_t id = 0;
            api.append_basic(v, T_I32, &id);
        });
    if (!std::strcmp(name, "ItemIsMenu"))
        return variant("b", [&](DBusMessageIter* v) {
            const dbus_bool_t no = 0;
            api.append_basic(v, T_BOOL, &no);
        });
    if (!std::strcmp(name, "Menu"))
        return variant("o", [&](DBusMessageIter* v) { api.append_basic(v, T_PATH, &kMenuPath); });
    if (!std::strcmp(name, "ToolTip"))  // (icon name, pixmaps, title, text)
        return variant("(sa(iiay)ss)", [&](DBusMessageIter* v) {
            DBusMessageIter st;
            api.open_container(v, T_STRUCT, nullptr, &st);
            put_str(api, &st, "phonemic");
            put_pixmaps(api, &st, {});
            put_str(api, &st, "PhoneMic");
            put_str(api, &st, "Your phone as a microphone");
            api.close_container(v, &st);
        });
    return false;
}

bool put_menu_prop(Api& api, DBusMessageIter* it, const char* name) {
    DBusMessageIter v;
    if (!std::strcmp(name, "Version")) {
        const uint32_t version = 3;
        api.open_container(it, T_VARIANT, "u", &v);
        api.append_basic(&v, T_U32, &version);
    } else if (!std::strcmp(name, "TextDirection") || !std::strcmp(name, "Status")) {
        api.open_container(it, T_VARIANT, "s", &v);
        put_str(api, &v, !std::strcmp(name, "Status") ? "normal" : "ltr");
    } else if (!std::strcmp(name, "IconThemePath")) {
        DBusMessageIter arr;
        api.open_container(it, T_VARIANT, "as", &v);
        api.open_container(&v, T_ARRAY, "s", &arr);
        api.close_container(&v, &arr);
    } else {
        return false;
    }
    api.close_container(it, &v);
    return true;
}

const char* const kItemProps[] = {"Category", "Id",       "Title",      "Status", "IconName",
                                  "IconPixmap", "WindowId", "ItemIsMenu", "Menu",   "ToolTip"};
const char* const kMenuProps[] = {"Version", "TextDirection", "Status", "IconThemePath"};

// org.freedesktop.DBus.Properties Get / GetAll for one of our objects.
int on_properties(Impl& t, DBusConnection* c, DBusMessage* m, bool menu) {
    Api& api = t.api;
    auto put = [&](DBusMessageIter* it, const char* name) {
        return menu ? put_menu_prop(api, it, name) : put_item_prop(t, it, name);
    };
    DBusError e;
    api.error_init(&e);
    const char* iface = nullptr;
    const char* name = nullptr;
    if (api.is_method_call(m, kPropsIface, "Get") &&
        api.get_args(m, &e, T_STRING, &iface, T_STRING, &name, T_INVALID)) {
        DBusMessage* r = api.new_method_return(m);
        DBusMessageIter it;
        api.iter_init_append(r, &it);
        if (put(&it, name)) {
            send_reply(api, c, r);
        } else {
            api.message_unref(r);
            send_reply(api, c, api.new_error(m, "org.freedesktop.DBus.Error.UnknownProperty", name));
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kPropsIface, "GetAll") && api.get_args(m, &e, T_STRING, &iface, T_INVALID)) {
        DBusMessage* r = api.new_method_return(m);
        DBusMessageIter it, dict;
        api.iter_init_append(r, &it);
        api.open_container(&it, T_ARRAY, "{sv}", &dict);
        auto all = [&](const auto& names) {
            for (const char* n : names) {
                DBusMessageIter entry;
                api.open_container(&dict, T_DICT_ENTRY, nullptr, &entry);
                put_str(api, &entry, n);
                put(&entry, n);
                api.close_container(&dict, &entry);
            }
        };
        if (!std::strcmp(iface, menu ? kMenuIface : kItemIface)) {
            if (menu)
                all(kMenuProps);
            else
                all(kItemProps);
        }
        api.close_container(&it, &dict);
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.error_is_set(&e)) api.error_free(&e);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int on_item_message(DBusConnection* c, DBusMessage* m, void* user) {
    Impl& t = *static_cast<Impl*>(user);
    if (t.api.is_method_call(m, kItemIface, "Activate")) {
        t.toggle = true;
    } else if (t.api.is_method_call(m, kItemIface, "SecondaryActivate")) {
        t.show = true;
    } else if (!t.api.is_method_call(m, kItemIface, "ContextMenu") && !t.api.is_method_call(m, kItemIface, "Scroll")) {
        return on_properties(t, c, m, false);
    }
    reply_ok(t.api, c, m);
    return DBUS_HANDLER_RESULT_HANDLED;
}

// A menu item's properties, a{sv}.
void put_menu_props(Api& api, DBusMessageIter* it, int32_t id) {
    DBusMessageIter props;
    api.open_container(it, T_ARRAY, "{sv}", &props);
    auto prop = [&](const char* key, const char* value) {
        DBusMessageIter entry, v;
        api.open_container(&props, T_DICT_ENTRY, nullptr, &entry);
        put_str(api, &entry, key);
        api.open_container(&entry, T_VARIANT, "s", &v);
        put_str(api, &v, value);
        api.close_container(&entry, &v);
        api.close_container(&props, &entry);
    };
    if (id == 0) prop("children-display", "submenu");
    if (id == kMenuShow) prop("label", "Show PhoneMic");
    if (id == kMenuQuit) prop("label", "Quit PhoneMic");
    if (id == kMenuSeparator) prop("type", "separator");
    api.close_container(it, &props);
}

// One menu item, (ia{sv}av); the root (id 0) carries the others as children.
void put_menu_item(Api& api, DBusMessageIter* it, int32_t id) {
    DBusMessageIter st, kids;
    api.open_container(it, T_STRUCT, nullptr, &st);
    api.append_basic(&st, T_I32, &id);
    put_menu_props(api, &st, id);
    api.open_container(&st, T_ARRAY, "v", &kids);
    if (id == 0) {
        for (const int32_t child : {kMenuShow, kMenuSeparator, kMenuQuit}) {
            DBusMessageIter v;
            api.open_container(&kids, T_VARIANT, "(ia{sv}av)", &v);
            put_menu_item(api, &v, child);
            api.close_container(&kids, &v);
        }
    }
    api.close_container(&st, &kids);
    api.close_container(it, &st);
}

void menu_clicked(Impl& t, int32_t id) {
    if (id == kMenuShow) t.show = true;
    if (id == kMenuQuit) t.quit = true;
}

int on_menu_message(DBusConnection* c, DBusMessage* m, void* user) {
    Impl& t = *static_cast<Impl*>(user);
    Api& api = t.api;
    DBusMessageIter in, out;
    if (api.is_method_call(m, kMenuIface, "GetLayout")) {  // (i parent, i depth, as props) -> u, (ia{sv}av)
        int32_t parent = 0;
        if (api.iter_init(m, &in) && api.arg_type(&in) == T_I32) api.get_basic(&in, &parent);
        DBusMessage* r = api.new_method_return(m);
        const uint32_t revision = 1;
        api.iter_init_append(r, &out);
        api.append_basic(&out, T_U32, &revision);
        put_menu_item(api, &out, parent);
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kMenuIface, "GetGroupProperties")) {  // (ai ids, as props) -> a(ia{sv})
        DBusMessage* r = api.new_method_return(m);
        DBusMessageIter arr, ids;
        api.iter_init_append(r, &out);
        api.open_container(&out, T_ARRAY, "(ia{sv})", &arr);
        if (api.iter_init(m, &in) && api.arg_type(&in) == T_ARRAY) {
            api.recurse(&in, &ids);
            for (; api.arg_type(&ids) == T_I32; api.next(&ids)) {
                int32_t id = 0;
                api.get_basic(&ids, &id);
                DBusMessageIter st;
                api.open_container(&arr, T_STRUCT, nullptr, &st);
                api.append_basic(&st, T_I32, &id);
                put_menu_props(api, &st, id);
                api.close_container(&arr, &st);
            }
        }
        api.close_container(&out, &arr);
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kMenuIface, "Event")) {  // (i id, s event, v data, u time)
        int32_t id = 0;
        const char* event = nullptr;
        if (api.iter_init(m, &in) && api.arg_type(&in) == T_I32) {
            api.get_basic(&in, &id);
            if (api.next(&in) && api.arg_type(&in) == T_STRING) api.get_basic(&in, &event);
        }
        if (event && !std::strcmp(event, "clicked")) menu_clicked(t, id);
        reply_ok(api, c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kMenuIface, "EventGroup")) {  // a(isvu) -> ai (ids not found)
        DBusMessageIter events, ev;
        if (api.iter_init(m, &in) && api.arg_type(&in) == T_ARRAY) {
            api.recurse(&in, &events);
            for (; api.arg_type(&events) == T_STRUCT; api.next(&events)) {
                api.recurse(&events, &ev);
                int32_t id = 0;
                const char* event = nullptr;
                api.get_basic(&ev, &id);
                if (api.next(&ev) && api.arg_type(&ev) == T_STRING) api.get_basic(&ev, &event);
                if (event && !std::strcmp(event, "clicked")) menu_clicked(t, id);
            }
        }
        DBusMessage* r = api.new_method_return(m);
        DBusMessageIter arr;
        api.iter_init_append(r, &out);
        api.open_container(&out, T_ARRAY, "i", &arr);
        api.close_container(&out, &arr);
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kMenuIface, "AboutToShow")) {  // -> b (needs update)
        DBusMessage* r = api.new_method_return(m);
        const dbus_bool_t no = 0;
        api.iter_init_append(r, &out);
        api.append_basic(&out, T_BOOL, &no);
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (api.is_method_call(m, kMenuIface, "AboutToShowGroup")) {  // -> ai updates, ai errors
        DBusMessage* r = api.new_method_return(m);
        api.iter_init_append(r, &out);
        for (int i = 0; i < 2; ++i) {
            DBusMessageIter arr;
            api.open_container(&out, T_ARRAY, "i", &arr);
            api.close_container(&out, &arr);
        }
        send_reply(api, c, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return on_properties(t, c, m, true);
}

// A second launch calls Show on the first one.
int on_instance_message(DBusConnection* c, DBusMessage* m, void* user) {
    Impl& t = *static_cast<Impl*>(user);
    if (!t.api.is_method_call(m, kInstanceName, "Show")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    t.show = true;
    reply_ok(t.api, c, m);
    return DBUS_HANDLER_RESULT_HANDLED;
}

std::string name_owner(Api& api, DBusConnection* c, const char* name) {
    DBusMessage* m =
        api.new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
    if (!m) return {};
    DBusMessageIter it;
    api.iter_init_append(m, &it);
    put_str(api, &it, name);
    DBusMessage* r = call(api, c, m, nullptr);
    if (!r) return {};  // NameHasNoOwner: no tray running
    std::string owner;
    const char* s = nullptr;
    if (api.iter_init(r, &it) && api.arg_type(&it) == T_STRING) api.get_basic(&it, &s);
    if (s) owner = s;
    api.message_unref(r);
    return owner;
}

bool register_with_watcher(Impl& t) {
    DBusMessage* m = t.api.new_method_call(kWatcherName, "/StatusNotifierWatcher", kWatcherName,
                                           "RegisterStatusNotifierItem");
    if (!m) return false;
    DBusMessageIter it;
    t.api.iter_init_append(m, &it);
    put_str(t.api, &it, t.item_name.c_str());
    DBusMessage* r = call(t.api, t.c, m, nullptr);
    if (!r) return false;
    t.api.message_unref(r);
    return true;
}

const DBusObjectPathVTable kItemVTable{nullptr, on_item_message, {}};
const DBusObjectPathVTable kMenuVTable{nullptr, on_menu_message, {}};
const DBusObjectPathVTable kInstanceVTable{nullptr, on_instance_message, {}};

}  // namespace

Tray::Tray() = default;
Tray::~Tray() { stop(); }

Tray::Start Tray::start(std::vector<TrayIcon> icons) {
    stop();
    impl_ = std::make_unique<Impl>();
    Impl& t = *impl_;
    Api& api = t.api;
    t.icons = std::move(icons);
    if (!api.load()) {
        impl_.reset();
        return Start::Unavailable;
    }
    DBusError e;
    api.error_init(&e);
    t.c = api.bus_get_private(DBUS_BUS_SESSION, &e);
    if (!t.c) {
        api.error_free(&e);
        impl_.reset();
        return Start::Unavailable;
    }
    api.set_exit_on_disconnect(t.c, 0);

    const int owner = api.request_name(t.c, kInstanceName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (api.error_is_set(&e)) {
        api.error_free(&e);  // carry on without single-instance
    } else if (owner != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        if (DBusMessage* m = api.new_method_call(kInstanceName, kInstancePath, kInstanceName, "Show"))
            if (DBusMessage* r = call(api, t.c, m, nullptr)) api.message_unref(r);
        stop();
        return Start::AlreadyRunning;
    }

    api.register_path(t.c, kInstancePath, &kInstanceVTable, &t, nullptr);
    api.register_path(t.c, kItemPath, &kItemVTable, &t, nullptr);
    api.register_path(t.c, kMenuPath, &kMenuVTable, &t, nullptr);
    t.item_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    api.request_name(t.c, t.item_name.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (api.error_is_set(&e)) api.error_free(&e);

    // Register now, so a close right after start-up already hides to the tray.
    std::string watcher = name_owner(api, t.c, kWatcherName);
    t.shown = !watcher.empty() && register_with_watcher(t);

    t.th = std::thread([&t, watcher]() mutable {
        uint64_t last_check = mono_ms();
        while (!t.stop.load()) {
            // The tray (e.g. plasmashell) may restart or appear later: register again.
            const uint64_t now = mono_ms();
            if (now - last_check >= 3000) {
                last_check = now;
                const std::string w = name_owner(t.api, t.c, kWatcherName);
                if (w != watcher) {
                    watcher = w;
                    t.shown = !w.empty() && register_with_watcher(t);
                }
            }
            if (!t.api.read_write_dispatch(t.c, 200)) break;  // the session bus went away
        }
        t.shown = false;
    });
    return Start::Running;
}

void Tray::stop() {
    if (!impl_) return;
    Impl& t = *impl_;
    t.stop = true;
    if (t.th.joinable()) t.th.join();
    if (t.c) {
        t.api.connection_close(t.c);
        t.api.connection_unref(t.c);
    }
    impl_.reset();
}

bool Tray::shown() const { return impl_ && impl_->shown.load(); }
bool Tray::take_toggle() { return impl_ && impl_->toggle.exchange(false); }
bool Tray::take_show() { return impl_ && impl_->show.exchange(false); }
bool Tray::take_quit() { return impl_ && impl_->quit.exchange(false); }

}  // namespace phonemic
