#include "app.h"
#include "app_icon.h"
#include "dev_paths.h"

#include "engine/version.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#include <timeapi.h>   // timeBeginPeriod
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// %LOCALAPPDATA%\TradeTerminal — runtime data (imgui.ini, later: config,
// strategy DLLs, cache) lives outside the OneDrive-synced source tree.
fs::path appdata_dir() {
    if (const char* base = std::getenv("LOCALAPPDATA"))
        return fs::path(base) / "TradeTerminal";
    return fs::temp_directory_path() / "TradeTerminal";
}

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

} // namespace

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1600, 900, "TradeTerminal", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync — the UI thread is not the hot path

    // Windows' default timer granularity is ~15.6 ms, which silently
    // stretches every Sleep() and socket timeout in the process. 1 ms
    // granularity keeps the engine's sleep tiers and the I/O threads'
    // wait caps meaning what they say.
    timeBeginPeriod(1);

    // Window/taskbar icon (GLFWimage.pixels wants non-const, hence the cast).
    GLFWimage icon_images[tt::ui::kAppIconCount];
    for (int i = 0; i < tt::ui::kAppIconCount; ++i) {
        const tt::ui::AppIconImage& src = tt::ui::kAppIcons[i];
        icon_images[i].width = src.width;
        icon_images[i].height = src.height;
        icon_images[i].pixels = const_cast<unsigned char*>(src.pixels);
    }
    glfwSetWindowIcon(window, tt::ui::kAppIconCount, icon_images);

#ifdef _WIN32
    // Match the OS-drawn titlebar to the app's dark ImGui theme.
    {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(glfwGetWin32Window(window), DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark, sizeof(dark));
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;

    const fs::path data_dir = appdata_dir();
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    static std::string ini_path = (data_dir / "imgui.ini").string();
    const bool had_ini = fs::exists(ini_path);
    io.IniFilename = ini_path.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // All market data + brokerage flows through the IBKR Client Portal
    // Gateway on this machine (log in via Account > Sign In > IBKR).
    // 127.0.0.1, not localhost: the gateway's Jetty often binds IPv4 only,
    // and localhost can resolve to ::1 first (connection refused).
    const char* gw_env = std::getenv("TT_IBKR_GATEWAY");
    std::string gateway_url = gw_env && *gw_env ? gw_env : "https://127.0.0.1:5000/v1/api";

    tt::ui::App app(gateway_url);
    app.set_had_ini(had_ini);
    app.log().add(std::string("TradeTerminal engine ") + tt::engine_version());
    app.log().add(std::string("ImGui ") + IMGUI_VERSION + " (docking) + ImPlot " + IMPLOT_VERSION);
    app.log().add("Runtime data: " + data_dir.string());

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.draw();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    timeEndPeriod(1);
    return 0;
}
