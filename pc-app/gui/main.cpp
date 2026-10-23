// PhoneMic desktop app — window + main loop (GLFW + OpenGL 3 + Dear ImGui).
// A real native window: no web view, no Electron. All audio work happens on
// the receiver / audio-device threads; this thread only draws (~30 fps, and it
// sleeps between frames instead of spinning).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.hpp"
#include "tray.hpp"
#include "ui.hpp"

namespace {

void glfw_error(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }

#if !defined(_WIN32)
// The window icon, drawn from packaging/linux/phonemic.svg's shapes as distance
// fields in its 256-unit space (Windows uses the .exe's icon resource instead).
float sd_round_box(float px, float py, float x0, float y0, float x1, float y1, float r) {
    const float qx = std::fabs(px - (x0 + x1) / 2) - ((x1 - x0) / 2 - r);
    const float qy = std::fabs(py - (y0 + y1) / 2) - ((y1 - y0) / 2 - r);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

// Distance to the arc of radius r around (cx, cy) whose ends are (cx +- ex, cy + ey).
float sd_arc(float px, float py, float cx, float cy, float r, float ex, float ey, bool below) {
    const bool inside = below ? py >= cy : std::fabs(py - cy) <= ey * std::hypot(px - cx, py - cy) / r && px > cx;
    if (inside) return std::fabs(std::hypot(px - cx, py - cy) - r);
    return std::min(sd_segment(px, py, cx + ex, cy + ey, cx + ex, cy + ey),
                    below ? sd_segment(px, py, cx - ex, cy + ey, cx - ex, cy + ey)
                          : sd_segment(px, py, cx + ex, cy - ey, cx + ex, cy - ey));
}

std::vector<unsigned char> make_icon(int n) {
    std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4, 0);
    const float unit = 256.0f / n;  // svg units per pixel
    auto cover = [&](float d) { return std::clamp(0.5f - d / unit, 0.0f, 1.0f); };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float u = (x + 0.5f) * unit, v = (y + 0.5f) * unit;
            const float bg = cover(sd_round_box(u, v, 8, 8, 248, 248, 56));
            if (bg <= 0) continue;
            float mic = cover(sd_round_box(u, v, 100, 40, 156, 148, 28));
            mic = std::max(mic, cover(sd_arc(u, v, 128, 116, 56, 56, 0, true) - 7));
            mic = std::max(mic, cover(sd_segment(u, v, 128, 172, 128, 200) - 7));
            mic = std::max(mic, cover(sd_segment(u, v, 100, 208, 156, 208) - 7));
            // Sound arcs (mirrored), at 55% like the svg.
            const float wave = 0.55f * cover(sd_arc(128 + std::fabs(u - 128), v, 128, 94, 92, 79.7f, 46, false) - 6);
            const float g = v / 256;  // #8F9DFF -> #5566E8, top to bottom
            const float rgb[3] = {0x8F + (0x55 - 0x8F) * g, 0x9D + (0x66 - 0x9D) * g, 0xFF + (0xE8 - 0xFF) * g};
            const float ink = std::max(mic, wave);
            unsigned char* p = &px[(static_cast<size_t>(y) * n + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = static_cast<unsigned char>(rgb[c] + (0x0B - rgb[c]) * ink + 0.5f);
            p[3] = static_cast<unsigned char>(255 * bg + 0.5f);
        }
    }
    return px;
}
#endif

}  // namespace

#if defined(_WIN32)
#include <windows.h>
int main(int, char**);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Run elevated by the "Rename to PhoneMic" button (rename_cable_async).
    if (argc == 2 && (std::strcmp(argv[1], "--rename-cable") == 0 || std::strcmp(argv[1], "--restore-cable") == 0))
        return phonemic::rename_cable_now(std::strcmp(argv[1], "--rename-cable") == 0);
#else
    (void)argc;
    (void)argv;
#endif
    // Before the window opens: a second launch only shows the running PhoneMic.
    phonemic::Tray tray;
#if defined(_WIN32)
    const auto started = tray.start({});  // the tray uses the .exe's icon resource
#else
    const auto started = tray.start({{22, make_icon(22)}, {32, make_icon(32)}, {64, make_icon(64)}});
#endif
    if (started == phonemic::Tray::Start::AlreadyRunning) {
        std::fprintf(stderr, "PhoneMic is already running; showing its window.\n");
        return 0;
    }
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "PhoneMic needs a graphical desktop (GLFW could not initialise).\n"
                             "On a headless machine use `phonemic-receiver` instead.\n");
        return 1;
    }
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* win = glfwCreateWindow(500, 820, "PhoneMic", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "Could not create an OpenGL 3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(win, 420, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win);
    // On Wayland a vsynced swap waits for the compositor's frame callback, which
    // may never come for a hidden, re-shown or minimised window and would freeze
    // the loop; glfwWaitEventsTimeout below paces the frames anyway.
    glfwSwapInterval(glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? 0 : 1);
#if !defined(_WIN32)
    {
        auto px = make_icon(64);
        GLFWimage img{64, 64, px.data()};
        glfwSetWindowIcon(win, 1, &img);
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing to persist; settings live in settings.ini
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(win, &xs, &ys);
    const float scale = xs > 0 ? xs : 1.0f;
    const float font_scale = scale;

    phonemic::UiState ui;
    ui.scale = scale;
    ui.fonts = phonemic::load_fonts(font_scale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        phonemic::App app;
        bool told_tray = false;
        auto show = [&] {
            glfwShowWindow(win);
            if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) glfwRestoreWindow(win);
            if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) glfwFocusWindow(win);  // Wayland can't take focus
        };
        double wait = 1.0 / 30.0, last_frame = 0;
        for (;;) {
            // Sleep until input arrives or ~33 ms pass (keeps the meter live at
            // ~30 fps without burning a CPU core).
            glfwWaitEventsTimeout(wait);
            app.tick();
            // With a tray icon, closing the window hides it and the mic keeps working;
            // "Quit PhoneMic" in the tray menu exits.
            if (glfwWindowShouldClose(win) && tray.shown()) {
                glfwSetWindowShouldClose(win, GLFW_FALSE);
                glfwHideWindow(win);
                if (!told_tray) app.log.add("PhoneMic keeps running in the system tray (Quit is in its menu)");
                told_tray = true;
            }
            if (tray.take_quit()) break;
            if (tray.take_toggle()) {
                if (glfwGetWindowAttrib(win, GLFW_VISIBLE) && !glfwGetWindowAttrib(win, GLFW_ICONIFIED))
                    glfwHideWindow(win);
                else
                    show();
            }
            if (tray.take_show()) show();
            if (!glfwGetWindowAttrib(win, GLFW_VISIBLE)) continue;  // hidden: keep the engine ticking, skip drawing
            if (glfwWindowShouldClose(win)) break;
            // At most ~60 fps: without vsync (Wayland) each frame's own buffer
            // release would wake the loop for the next one. Skipped input waits
            // in ImGui's queue for the next frame.
            const double now = glfwGetTime();
            if (now - last_frame < 1.0 / 60.0) {
                wait = 1.0 / 60.0 - (now - last_frame);
                continue;
            }
            wait = 1.0 / 30.0;
            last_frame = now;
            if (ui.theme != app.settings.theme || ui.accent != app.settings.accent) {
                ui.theme = app.settings.theme;
                ui.accent = app.settings.accent;
                phonemic::setup_style(scale, ui.theme, ui.accent);
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::PushFont(ui.fonts.body);
            phonemic::draw_ui(app, ui);
            ImGui::PopFont();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            glViewport(0, 0, w, h);
            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            glClearColor(bg.x, bg.y, bg.z, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(win);
        }
    }  // App shuts down (stops threads, removes the Linux virtual mic, saves settings).

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
