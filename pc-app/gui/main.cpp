// PhoneMic desktop app — window + main loop (GLFW + OpenGL 3 + Dear ImGui).
// A real native window: no web view, no Electron. All audio work happens on
// the receiver / audio-device threads; this thread only draws (~30 fps, and it
// sleeps between frames instead of spinning).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.hpp"
#include "ui.hpp"

namespace {

void glfw_error(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }

// A 64x64 window icon drawn procedurally (accent disc, black microphone).
std::vector<unsigned char> make_icon(int n) {
    std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4, 0);
    const float c = n / 2.0f, r = n / 2.0f - 1;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float dx = x + 0.5f - c, dy = y + 0.5f - c;
            unsigned char* p = &px[(static_cast<size_t>(y) * n + x) * 4];
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > r) continue;
            const float a = std::min(1.0f, r - d);
            bool glyph = false;
            // Capsule.
            const float cw = n * 0.11f, top = c - n * 0.28f, bot = c + n * 0.06f;
            if (std::fabs(dx) <= cw && dy + c >= top + cw && dy + c <= bot) glyph = true;
            for (float cy : {top + cw, bot}) {
                const float ex = dx, ey = dy + c - cy;
                if (ex * ex + ey * ey <= cw * cw) glyph = true;
            }
            // Holder arc + stem.
            const float ad = std::sqrt(dx * dx + (dy + n * 0.02f) * (dy + n * 0.02f));
            if (dy > -n * 0.02f && std::fabs(ad - n * 0.19f) < n * 0.03f) glyph = true;
            if (std::fabs(dx) < n * 0.03f && dy > n * 0.17f && dy < n * 0.29f) glyph = true;
            p[0] = glyph ? 0 : 0x7A;
            p[1] = glyph ? 0 : 0x8C;
            p[2] = glyph ? 0 : 0xFF;
            p[3] = static_cast<unsigned char>(255 * a);
        }
    }
    return px;
}

}  // namespace

#if defined(_WIN32)
#include <windows.h>
int main(int, char**);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif

int main(int, char**) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "PhoneMic needs a graphical desktop (GLFW could not initialise).\n"
                             "On a headless machine use `phonemic-receiver` instead.\n");
        return 1;
    }
#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* win = glfwCreateWindow(500, 820, "PhoneMic", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "Could not create an OpenGL 3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(win, 420, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
#if !defined(__APPLE__)
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
#if defined(__APPLE__)
    const float scale = 1.0f;  // macOS scales the framebuffer itself
    io.FontGlobalScale = 1.0f / xs;
    const float font_scale = xs;
#else
    const float scale = xs > 0 ? xs : 1.0f;
    const float font_scale = scale;
#endif

    phonemic::UiState ui;
    ui.scale = scale;
    phonemic::setup_style(scale);
    ui.fonts = phonemic::load_fonts(font_scale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        phonemic::App app;
        while (!glfwWindowShouldClose(win)) {
            // Sleep until input arrives or ~33 ms pass (keeps the meter live at
            // ~30 fps without burning a CPU core).
            glfwWaitEventsTimeout(1.0 / 30.0);
            app.tick();
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
            glClearColor(0, 0, 0, 1);  // OLED black
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
