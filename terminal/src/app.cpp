#include "app.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout)
#include "implot.h"

#include <cmath>
#include <vector>

namespace tt::ui {

void App::draw() {
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    draw_menu_bar();
    if (show_chart_) draw_chart_placeholder();
    if (show_log_) log_.draw("Log Console", &show_log_);
    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Log Console", nullptr, &show_log_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImPlot Demo", nullptr, &show_implot_demo_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::draw_chart_placeholder() {
    if (!ImGui::Begin("Chart", &show_chart_)) {
        ImGui::End();
        return;
    }

    // Placeholder random walk until Phase 1 wires real candles in.
    static std::vector<double> xs, ys;
    if (xs.empty()) {
        double px = 100.0;
        unsigned rng = 0x5eed;
        for (int i = 0; i < 500; ++i) {
            rng = rng * 1664525u + 1013904223u;
            px += (static_cast<double>(rng >> 8 & 0xffff) / 65535.0 - 0.5) * 2.0;
            xs.push_back(i);
            ys.push_back(px);
        }
    }

    ImGui::TextDisabled("Placeholder data — Phase 1 connects the data service.");
    if (ImPlot::BeginPlot("##placeholder", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("bar", "price");
        ImPlot::PlotLine("SIM", xs.data(), ys.data(), static_cast<int>(xs.size()));
        ImPlot::EndPlot();
    }
    ImGui::End();
}

void App::setup_default_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Chart", center);
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
