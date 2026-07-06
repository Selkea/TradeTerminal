#include "panels/strategy_mgr.h"

#include "imgui.h"
#include "ui_hints.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace tt::ui {

StrategyManagerPanel::StrategyManagerPanel(StrategyHost& host, Engine& eng,
                                           std::string strategies_dir)
    : host_(host), eng_(eng), dir_(std::move(strategies_dir)) {
    host_.sweep_stale();
    use_builtin_params();
    refresh_files();
}

StrategyManagerPanel::~StrategyManagerPanel() {
    if (build_thread_.joinable()) build_thread_.join();
}

void StrategyManagerPanel::use_builtin_params() {
    params_ = {{"fast", 10, 1, 500, 10},
               {"slow", 30, 2, 1000, 30},
               {"qty", 100, 1, 100000, 100}};
}

void StrategyManagerPanel::adopt_loaded_params() {
    params_.clear();
    if (const StrategyHost::Loaded* l = host_.current())
        for (const auto& p : l->params)
            params_.push_back(ParamValue{p.name, p.def, p.min, p.max, p.def});
}

void StrategyManagerPanel::console(std::string line) {
    std::lock_guard lock(out_mu_);
    output_.push_back(std::move(line));
    while (output_.size() > 500) output_.pop_front();
}

void StrategyManagerPanel::refresh_files() {
    files_.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".cpp")
            files_.push_back(e.path().filename().string());
    if (selected_ >= static_cast<int>(files_.size()))
        selected_ = files_.empty() ? -1 : 0;
}

void StrategyManagerPanel::start_build(const std::string& src) {
    if (building_.exchange(true)) return;
    {
        std::lock_guard lock(out_mu_);
        output_.clear();
    }
    if (build_thread_.joinable()) build_thread_.join();
    build_thread_ = std::thread([this, src] {
        std::string dll;
        const bool ok = host_.compile(src, [this](std::string l) { console(std::move(l)); }, dll);
        if (ok) {
            std::lock_guard lock(pending_mu_);
            pending_dll_ = dll;
            pending_src_ = src;
        }
        building_ = false;
    });
}

IStrategy* StrategyManagerPanel::active_strategy(IStrategy& builtin) {
    if (const StrategyHost::Loaded* l = host_.current()) return l->instance;
    return &builtin;
}

std::string StrategyManagerPanel::active_name() const {
    if (const StrategyHost::Loaded* l = host_.current()) return l->name;
    return "SMA Crossover (built-in)";
}

std::map<std::string, double> StrategyManagerPanel::param_values() const {
    std::map<std::string, double> out;
    for (const auto& p : params_) out[p.name] = p.value;
    return out;
}

void StrategyManagerPanel::draw(bool* open) {
    const bool visible = ImGui::Begin("Strategy", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    // Load freshly built DLLs once the engine is idle (never mid-run).
    {
        std::string dll, src;
        {
            std::lock_guard lock(pending_mu_);
            dll.swap(pending_dll_);
            src.swap(pending_src_);
        }
        if (!dll.empty()) {
            // "Idle" must include live sessions: the live thread calls into
            // the currently loaded DLL, and load() starts by unloading it.
            if (eng_.running() || eng_.live_running()) {
                std::lock_guard lock(pending_mu_);   // put it back, retry next frame
                pending_dll_ = std::move(dll);
                pending_src_ = std::move(src);
            } else {
                std::string err;
                if (host_.load(dll, src, err)) {
                    adopt_loaded_params();
                    console("loaded: " + host_.current()->name);
                } else {
                    console("load failed: " + err);
                }
            }
        }
    }

    // Periodic directory refresh.
    const double now = ImGui::GetTime();
    if (now > next_refresh_s_) {
        next_refresh_s_ = now + 3.0;
        refresh_files();
    }

    ImGui::TextDisabled("Active: %s", active_name().c_str());
    ImGui::Separator();

    ImGui::SetNextItemWidth(-100);
    if (ImGui::BeginCombo("##srcfile",
                          selected_ >= 0 && selected_ < static_cast<int>(files_.size())
                              ? files_[selected_].c_str()
                              : "(no .cpp files)")) {
        for (int i = 0; i < static_cast<int>(files_.size()); ++i)
            if (ImGui::Selectable(files_[i].c_str(), i == selected_)) selected_ = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    const bool can_build = !building_ && selected_ >= 0 && !eng_.running() &&
                           !eng_.live_running();
    ImGui::BeginDisabled(!can_build);
    if (ImGui::Button(building_ ? "Building..." : "Build & Load"))
        start_build((fs::path(dir_) / files_[selected_]).string());
    ImGui::EndDisabled();

    if (host_.current()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(eng_.running() || eng_.live_running());
        if (ImGui::Button("Unload")) {
            host_.unload();
            use_builtin_params();
            console("unloaded; built-in SMA active");
        }
        ImGui::EndDisabled();
    }

    // Parameter editor for the active strategy.
    if (!params_.empty()) {
        ImGui::SeparatorText("Parameters");
        for (auto& p : params_) {
            ImGui::SetNextItemWidth(140);
            ImGui::DragScalar(p.name.c_str(), ImGuiDataType_Double, &p.value, 1.0f,
                              &p.min, &p.max, "%.4g");
        }
        if (ImGui::SmallButton("Reset defaults"))
            for (auto& p : params_) p.value = p.def;
    }

    // Compiler console.
    ImGui::SeparatorText("Compiler output");
    if (ImGui::BeginChild("##cc", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard lock(out_mu_);
        for (const auto& l : output_) {
            const bool is_err = l.find("error") != std::string::npos;
            if (is_err)
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1.0f), "%s", l.c_str());
            else
                ImGui::TextUnformatted(l.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace tt::ui
