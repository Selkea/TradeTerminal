#include "panels/strategy_mgr.h"

#include "imgui.h"
#include "ui_hints.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace tt::ui {

namespace {
constexpr const char* kBuiltinLabel = "(built-in SMA)";
}

StrategyManagerPanel::StrategyManagerPanel(StrategyHost& host, Engine& eng,
                                           std::string strategies_dir)
    : host_(host), eng_(eng), dir_(std::move(strategies_dir)) {
    host_.sweep_stale();
    param_vals_[""] = {{"fast", 10, 1, 500, 10},
                       {"slow", 30, 2, 1000, 30},
                       {"qty", 100, 1, 100000, 100}};
    refresh_files();
}

StrategyManagerPanel::~StrategyManagerPanel() {
    if (build_thread_.joinable()) build_thread_.join();
}

void StrategyManagerPanel::adopt_params(const std::string& key) {
    StrategyHost::ModuleView mv;
    if (!host_.info(key, mv)) return;
    std::vector<ParamValue> fresh;
    fresh.reserve(mv.params.size());
    const auto old = param_vals_.find(key);
    for (const auto& p : mv.params) {
        double value = p.def;
        // A rebuild keeps the user's edited value when the param survived.
        if (old != param_vals_.end())
            for (const auto& o : old->second)
                if (o.name == p.name) value = o.value;
        if (value < p.min) value = p.min;
        if (value > p.max) value = p.max;
        fresh.push_back(ParamValue{p.name, p.def, p.min, p.max, value});
    }
    param_vals_[key] = std::move(fresh);
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
    if (selected_ > static_cast<int>(files_.size())) selected_ = 0;
}

void StrategyManagerPanel::start_build(const std::string& src, bool make_active) {
    if (building_.exchange(true)) return;
    {
        std::lock_guard lock(out_mu_);
        output_.clear();
    }
    if (build_thread_.joinable()) build_thread_.join();
    {
        std::lock_guard lock(pending_mu_);
        pending_make_active_ = make_active;
    }
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

// Deferred work: queued build requests, then freshly built DLLs. Loading no
// longer needs the engine idle — replacing a module retires the old one, and
// its DLL lives until the last running instance is destroyed.
void StrategyManagerPanel::pump() {
    if (!want_load_.empty() && !building_.load()) {
        const std::string want = want_load_;
        want_load_.clear();
        if (!loaded_fresh(want))
            start_build((fs::path(dir_) / want).string(), /*make_active=*/false);
    }

    std::string dll, src;
    bool make_active = false;
    {
        std::lock_guard lock(pending_mu_);
        dll.swap(pending_dll_);
        src.swap(pending_src_);
        make_active = pending_make_active_;
    }
    if (dll.empty()) return;
    std::string err;
    if (host_.load(dll, src, err)) {
        const std::string key = fs::path(src).filename().string();
        adopt_params(key);
        if (make_active) active_key_ = key;
        console("loaded: " + display_name(key) + (make_active ? " (active)" : ""));
    } else {
        console("load failed: " + err);
    }
}

std::string StrategyManagerPanel::display_name(const std::string& key) const {
    if (key.empty()) return "SMA Crossover (built-in)";
    StrategyHost::ModuleView mv;
    if (host_.info(key, mv)) return mv.name;
    return key;
}

std::map<std::string, double> StrategyManagerPanel::param_values(
    const std::string& key) const {
    std::map<std::string, double> out;
    const auto it = param_vals_.find(key);
    if (it != param_vals_.end()) {
        for (const auto& p : it->second) out[p.name] = p.value;
        return out;
    }
    StrategyHost::ModuleView mv;   // loaded but never edited: defaults
    if (host_.info(key, mv))
        for (const auto& p : mv.params) out[p.name] = p.def;
    return out;
}

bool StrategyManagerPanel::loaded_fresh(const std::string& key) const {
    if (key.empty()) return true;   // built-in never builds
    return host_.has(key) && !host_.stale(key);
}

void StrategyManagerPanel::request_load(const std::string& key) {
    if (key.empty() || loaded_fresh(key)) return;
    if (building_.load()) {
        want_load_ = key;
        return;
    }
    start_build((fs::path(dir_) / key).string(), /*make_active=*/false);
}

bool StrategyManagerPanel::load_pending() const {
    if (building_.load() || !want_load_.empty()) return true;
    std::lock_guard lock(pending_mu_);
    return !pending_dll_.empty();
}

std::vector<StrategyManagerPanel::ParamValue>* StrategyManagerPanel::editor_params(
    const std::string& key) {
    const auto it = param_vals_.find(key);
    if (it != param_vals_.end()) return &it->second;
    if (!key.empty() && host_.has(key)) {
        adopt_params(key);
        return &param_vals_[key];
    }
    return nullptr;
}

void StrategyManagerPanel::draw(bool* open) {
    const bool visible = ImGui::Begin("Strategy", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    // Periodic directory refresh.
    const double now = ImGui::GetTime();
    if (now > next_refresh_s_) {
        next_refresh_s_ = now + 3.0;
        refresh_files();
    }

    ImGui::TextDisabled("Active (live/replay/trade): %s", active_name().c_str());
    ImGui::Separator();

    // Source picker: entry 0 is the built-in, the rest are .cpp files.
    const std::string sel_key =
        selected_ > 0 && selected_ <= static_cast<int>(files_.size())
            ? files_[static_cast<size_t>(selected_ - 1)]
            : std::string();
    ImGui::SetNextItemWidth(-100);
    if (ImGui::BeginCombo("##srcfile",
                          sel_key.empty() ? kBuiltinLabel : sel_key.c_str())) {
        if (ImGui::Selectable(kBuiltinLabel, selected_ == 0)) selected_ = 0;
        for (int i = 0; i < static_cast<int>(files_.size()); ++i)
            if (ImGui::Selectable(files_[static_cast<size_t>(i)].c_str(),
                                  i + 1 == selected_))
                selected_ = i + 1;
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    if (sel_key.empty()) {
        ImGui::BeginDisabled(active_key_.empty());
        if (ImGui::Button("Set active")) {
            active_key_.clear();
            console("built-in SMA active");
        }
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(building_.load());
        if (ImGui::Button(building_.load() ? "Building..." : "Build & Load"))
            start_build((fs::path(dir_) / sel_key).string(), /*make_active=*/true);
        ImGui::EndDisabled();
    }

    // Loaded modules: instances in use, activation, unload (safe anytime —
    // running instances keep the retired DLL alive until they finish).
    const std::vector<StrategyHost::ModuleView> mods = host_.modules();
    if (!mods.empty()) {
        ImGui::SeparatorText("Loaded");
        for (const auto& m : mods) {
            ImGui::PushID(m.key.c_str());
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::SameLine();
            if (m.key == active_key_) {
                ImGui::TextDisabled("(%s, active)", m.key.c_str());
            } else {
                ImGui::TextDisabled("(%s)", m.key.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Set active")) {
                    active_key_ = m.key;
                    console(m.name + " active");
                }
            }
            if (m.instances > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%d in use", m.instances);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Unload")) {
                host_.unload(m.key);
                if (active_key_ == m.key) active_key_.clear();
                console("unloaded " + m.name +
                        (m.instances > 0 ? " (freed when its runs end)" : ""));
            }
            ImGui::PopID();
        }
    }

    // Parameter editor for the strategy selected above.
    if (std::vector<ParamValue>* params = editor_params(sel_key)) {
        ImGui::SeparatorText(
            ("Parameters — " + display_name(sel_key)).c_str());
        for (auto& p : *params) {
            ImGui::SetNextItemWidth(140);
            ImGui::DragScalar(p.name.c_str(), ImGuiDataType_Double, &p.value, 1.0f,
                              &p.min, &p.max, "%.4g");
        }
        if (ImGui::SmallButton("Reset defaults"))
            for (auto& p : *params) p.value = p.def;
    } else if (!sel_key.empty()) {
        ImGui::TextDisabled("Build & Load to edit this strategy's parameters.");
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
