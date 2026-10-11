#include "phonemic/pulse_ctl.hpp"

#if defined(__linux__)

#include <dlfcn.h>

#include <chrono>
#include <type_traits>

namespace phonemic {

namespace {
// The few libpulse entry points we use, declared here so no headers are needed.
// Signatures and enum values are libpulse's stable public ABI (pulse/context.h,
// pulse/introspect.h, pulse/mainloop.h, pulse/operation.h).
struct pa_mainloop;
struct pa_mainloop_api;
struct pa_context;
struct pa_operation;
struct pa_info;  // pa_sink_info / pa_source_info / pa_server_info: only the
                 // server name field is read, through server_name_offset below

using index_cb = void (*)(pa_context*, uint32_t, void*);
using success_cb = void (*)(pa_context*, int, void*);
using info_cb = void (*)(pa_context*, const pa_info*, int eol, void*);
using server_cb = void (*)(pa_context*, const void* server_info, void*);

constexpr int PA_CONTEXT_READY = 4, PA_CONTEXT_FAILED = 5, PA_CONTEXT_TERMINATED = 6;
constexpr int PA_OPERATION_RUNNING = 0;
constexpr int PA_CONTEXT_NOAUTOSPAWN = 1;

struct Api {
    void* lib = nullptr;
    pa_mainloop* (*mainloop_new)();
    pa_mainloop_api* (*mainloop_get_api)(pa_mainloop*);
    int (*mainloop_iterate)(pa_mainloop*, int block, int* retval);
    void (*mainloop_free)(pa_mainloop*);
    pa_context* (*context_new)(pa_mainloop_api*, const char*);
    int (*context_connect)(pa_context*, const char* server, int flags, const void* api);
    int (*context_get_state)(const pa_context*);
    void (*context_disconnect)(pa_context*);
    void (*context_unref)(pa_context*);
    pa_operation* (*load_module)(pa_context*, const char*, const char*, index_cb, void*);
    pa_operation* (*unload_module)(pa_context*, uint32_t, success_cb, void*);
    pa_operation* (*sink_by_name)(pa_context*, const char*, info_cb, void*);
    pa_operation* (*source_by_name)(pa_context*, const char*, info_cb, void*);
    pa_operation* (*server_info)(pa_context*, server_cb, void*);
    int (*operation_get_state)(const pa_operation*);
    void (*operation_unref)(pa_operation*);

    bool load() {
        lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn != nullptr;
        };
        sym(mainloop_new, "pa_mainloop_new");
        sym(mainloop_get_api, "pa_mainloop_get_api");
        sym(mainloop_iterate, "pa_mainloop_iterate");
        sym(mainloop_free, "pa_mainloop_free");
        sym(context_new, "pa_context_new");
        sym(context_connect, "pa_context_connect");
        sym(context_get_state, "pa_context_get_state");
        sym(context_disconnect, "pa_context_disconnect");
        sym(context_unref, "pa_context_unref");
        sym(load_module, "pa_context_load_module");
        sym(unload_module, "pa_context_unload_module");
        sym(sink_by_name, "pa_context_get_sink_info_by_name");
        sym(source_by_name, "pa_context_get_source_info_by_name");
        sym(server_info, "pa_context_get_server_info");
        sym(operation_get_state, "pa_operation_get_state");
        sym(operation_unref, "pa_operation_unref");
        if (!ok) {
            dlclose(lib);
            lib = nullptr;
        }
        return ok;
    }
};

constexpr auto kTimeout = std::chrono::seconds(5);
}  // namespace

struct PulseCtl::Impl {
    Api api;
    pa_mainloop* ml = nullptr;
    pa_context* ctx = nullptr;

    ~Impl() {
        if (ctx) {
            api.context_disconnect(ctx);
            api.context_unref(ctx);
        }
        if (ml) api.mainloop_free(ml);
        if (api.lib) dlclose(api.lib);
    }

    // Run the loop until the operation finishes (or times out).
    bool wait(pa_operation* op) {
        if (!op) return false;
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (api.operation_get_state(op) == PA_OPERATION_RUNNING) {
            if (std::chrono::steady_clock::now() > deadline ||
                api.context_get_state(ctx) != PA_CONTEXT_READY || api.mainloop_iterate(ml, 1, nullptr) < 0) {
                api.operation_unref(op);
                return false;
            }
        }
        api.operation_unref(op);
        return true;
    }
};

std::unique_ptr<PulseCtl> PulseCtl::connect(std::string* err) {
    auto impl = std::make_unique<Impl>();
    if (!impl->api.load()) {
        if (err) *err = "libpulse is not installed";
        return nullptr;
    }
    Api& a = impl->api;
    impl->ml = a.mainloop_new();
    if (!impl->ml) return nullptr;
    impl->ctx = a.context_new(a.mainloop_get_api(impl->ml), "PhoneMic");
    if (!impl->ctx || a.context_connect(impl->ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        if (err) *err = "no PulseAudio/PipeWire sound server is running";
        return nullptr;
    }
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    for (;;) {
        const int st = a.context_get_state(impl->ctx);
        if (st == PA_CONTEXT_READY) break;
        if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED || std::chrono::steady_clock::now() > deadline ||
            a.mainloop_iterate(impl->ml, 1, nullptr) < 0) {
            if (err) *err = "no PulseAudio/PipeWire sound server is running";
            return nullptr;
        }
    }
    std::unique_ptr<PulseCtl> c(new PulseCtl());
    c->impl_ = std::move(impl);
    return c;
}

PulseCtl::~PulseCtl() = default;

bool PulseCtl::has_sink(const std::string& name) {
    bool found = false;
    impl_->wait(impl_->api.sink_by_name(
        impl_->ctx, name.c_str(),
        [](pa_context*, const pa_info* i, int eol, void* ud) {
            if (!eol && i) *static_cast<bool*>(ud) = true;
        },
        &found));
    return found;
}

bool PulseCtl::has_source(const std::string& name) {
    bool found = false;
    impl_->wait(impl_->api.source_by_name(
        impl_->ctx, name.c_str(),
        [](pa_context*, const pa_info* i, int eol, void* ud) {
            if (!eol && i) *static_cast<bool*>(ud) = true;
        },
        &found));
    return found;
}

uint32_t PulseCtl::load_module(const std::string& name, const std::string& args) {
    uint32_t idx = kInvalid;
    impl_->wait(impl_->api.load_module(
        impl_->ctx, name.c_str(), args.c_str(),
        [](pa_context*, uint32_t i, void* ud) { *static_cast<uint32_t*>(ud) = i; }, &idx));
    return idx;
}

bool PulseCtl::unload_module(uint32_t index) {
    int ok = 0;
    impl_->wait(impl_->api.unload_module(
        impl_->ctx, index, [](pa_context*, int success, void* ud) { *static_cast<int*>(ud) = success; }, &ok));
    return ok != 0;
}

std::string PulseCtl::server_name() {
    // pa_server_info starts with: const char* user_name, host_name,
    // server_version, server_name (stable since PulseAudio 0.9). PipeWire
    // reports e.g. "PulseAudio (on PipeWire 1.0.5)".
    std::string out;
    impl_->wait(impl_->api.server_info(
        impl_->ctx,
        [](pa_context*, const void* info, void* ud) {
            if (!info) return;
            const char* const* f = static_cast<const char* const*>(info);
            if (f[3]) *static_cast<std::string*>(ud) = f[3];
        },
        &out));
    return out;
}

}  // namespace phonemic

#endif
