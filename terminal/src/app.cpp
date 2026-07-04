#include "app.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout)
#include "implot.h"

namespace tt::ui {

App::App(std::string python_cmd, std::string service_dir)
    : ipc_(std::move(python_cmd), std::move(service_dir)),
      chart_(ipc_, series_),
      watchlist_(ipc_, quotes_) {
    net::IpcClient::Callbacks cbs;
    cbs.on_log = [this](std::string line) { log_.add(std::move(line)); };
    cbs.on_tick = [this](const std::string& sym, const Quote& q) { quotes_.set(sym, q); };
    cbs.on_error = [this](uint32_t id, std::string code, std::string msg) {
        log_.add("feed error (req " + std::to_string(id) + ") " + code + ": " + msg);
    };
    cbs.on_candles = [this](net::CandleBatch&& b) {
        log_.add("candles: " + b.symbol + " " + b.interval + " x" +
                 std::to_string(b.candles.size()) + (b.cached ? " (cache)" : ""));
        series_.put(b.symbol, b.interval, std::move(b.candles), b.cached);
    };
    ipc_.start(std::move(cbs));
}

App::~App() { ipc_.stop(); }

void App::draw() {
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    draw_menu_bar();
    if (show_chart_) chart_.draw(&show_chart_);
    if (show_watchlist_)
        watchlist_.draw(&show_watchlist_,
                        [this](const std::string& sym) { chart_.show_symbol(sym); });
    if (show_log_) log_.draw("Log Console", &show_log_);
    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Watchlist", nullptr, &show_watchlist_);
        ImGui::MenuItem("Log Console", nullptr, &show_log_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImPlot Demo", nullptr, &show_implot_demo_);
        ImGui::EndMenu();
    }

    // Right-aligned feed health indicator.
    const bool up = ipc_.connected();
    const char* label = up ? "FEED UP" : "FEED DOWN";
    const float w = ImGui::CalcTextSize(label).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextColored(up ? ImVec4(0.25f, 0.85f, 0.45f, 1.0f)
                          : ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "%s", label);
    ImGui::EndMainMenuBar();
}

void App::setup_default_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Watchlist", left);
    ImGui::DockBuilderDockWindow("Chart", center);
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
