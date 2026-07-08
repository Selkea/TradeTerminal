#include "panels/replay.h"

#include "imgui.h"
#include "ui_hints.h"

#include <algorithm>
#include <filesystem>

namespace tt::ui {

void ReplayPanel::scan_files() {
    files_.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(sessions_dir_, ec))
        if (e.is_regular_file() && e.path().extension() == ".ttk")
            files_.push_back(e.path().filename().string());
    std::sort(files_.rbegin(), files_.rend());   // newest first
    file_idx_ = 0;
}

void ReplayPanel::draw(bool* open, const std::vector<std::string>& strat_keys,
                       const NameFn& name, const ReplayFn& replay) {
    const bool visible = ImGui::Begin("Replay", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    if (!scanned_) {
        scan_files();
        scanned_ = true;
    }

    if (files_.empty()) {
        ImGui::TextDisabled("No .ttk recordings yet — enable Record in the Trade "
                            "panel and run a session.");
        if (ImGui::SmallButton("refresh")) scan_files();
        ImGui::End();
        return;
    }

    if (file_idx_ >= static_cast<int>(files_.size())) file_idx_ = 0;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("recording", files_[file_idx_].c_str())) {
        for (int i = 0; i < static_cast<int>(files_.size()); ++i)
            if (ImGui::Selectable(files_[i].c_str(), i == file_idx_)) file_idx_ = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh")) scan_files();

    // Strategy pick (loaded modules only; build via the Strategy panel). A pick
    // that isn't loaded right now is kept — the startup restore rebuilds
    // strategies asynchronously, and running it too early just logs an error.
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("strategy", name(strat_key_).c_str())) {
        if (ImGui::Selectable(name("").c_str(), strat_key_.empty())) strat_key_.clear();
        for (const std::string& k : strat_keys) {
            const std::string lbl = name(k) + "###" + k;
            if (ImGui::Selectable(lbl.c_str(), k == strat_key_)) strat_key_ = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Strategy the recording replays through (loaded strategies "
                          "only — build via the Strategy panel)");

    ImGui::SetNextItemWidth(100);
    ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");
    ImGui::SetItemTooltip("Starting simulator balance for the re-run");
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("bar sec", &bar_sec_, 0, 0);
    bar_sec_ = std::max(0, bar_sec_);
    ImGui::SetItemTooltip("Re-bar the recording at this many seconds for on_bar "
                          "strategies. 0 = use the size it was recorded with.");

    ImGui::BeginDisabled(eng_.running());
    if (ImGui::Button("Replay", ImVec2(-1, 0)) && replay)
        replay(sessions_dir_ + "\\" + files_[file_idx_], bar_sec_, strat_key_, cash_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Re-run these captured ticks through the strategy + fill "
                          "simulator (deterministic; results land in Backtest)");

    ImGui::End();
}

} // namespace tt::ui
