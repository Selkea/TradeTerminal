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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// ImPlot doesn't persist its style/colors to imgui.ini (only axis state), so the
// Style Editor's changes reset each launch. Save/restore the whole ImPlotStyle
// (POD: color array + scalars, no pointers) as a binary blob, length-guarded so
// a different ImPlot build is ignored rather than misread.
void save_implot_style(const fs::path& path) {
    const ImPlotStyle& s = ImPlot::GetStyle();
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const uint32_t sz = sizeof(ImPlotStyle);
    f.write(reinterpret_cast<const char*>(&sz), sizeof sz);
    f.write(reinterpret_cast<const char*>(&s), sizeof s);
}
void load_implot_style(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    uint32_t sz = 0;
    f.read(reinterpret_cast<char*>(&sz), sizeof sz);
    if (!f || sz != sizeof(ImPlotStyle)) return;   // absent or different ImPlot build
    ImPlotStyle& s = ImPlot::GetStyle();
    f.read(reinterpret_cast<char*>(&s), sizeof s);
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

    static std::string implot_style_path = (data_dir / "implot_style.dat").string();
    load_implot_style(implot_style_path);   // restore saved ImPlot colors/style

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

    while (!app.should_quit()) {
        glfwPollEvents();
        // The window X only requests a close; the app quits immediately when
        // nothing is trading, or after confirming + safely stopping a live
        // session. Reset the GLFW flag so the loop keeps rendering the dialog.
        if (glfwWindowShouldClose(window)) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            app.request_quit();
        }
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

    // Flush ImGui + ImPlot settings (window layout, plot axis state) to imgui.ini
    // while BOTH contexts are still alive — ImPlot's settings handler needs its
    // context, and it would otherwise be gone before ImGui's own shutdown save.
    ImGui::SaveIniSettingsToDisk(ini_path.c_str());
    io.IniFilename = nullptr;   // prevent a second save after ImPlot is destroyed
    save_implot_style(implot_style_path);   // persist ImPlot colors/style

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    timeEndPeriod(1);
    return 0;
}
