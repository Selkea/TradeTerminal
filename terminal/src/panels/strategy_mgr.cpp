#include "panels/strategy_mgr.h"

#include "imgui.h"
#include "imgui_internal.h"   // GetCurrentTabBar: overflow-aware tab-list button
#include "ui_hints.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace tt::ui {


StrategyManagerPanel::StrategyManagerPanel(StrategyHost& host, Engine& eng,
                                           std::string strategies_dir)
    : host_(host), eng_(eng), dir_(std::move(strategies_dir)) {
    host_.sweep_stale();
    param_vals_[""] = {{"fast", 10, 1, 500, 10},
                       {"slow", 30, 2, 1000, 30},
                       {"qty", 100, 1, 100000, 100}};
    for (const auto& e : tt::static_strategy_registry())
        if (!static_keys_.insert(e.key).second)
            console("WARNING: duplicate promoted-strategy key '" + std::string(e.key) +
                    "' -- check terminal/CMakeLists.txt's TT_PROMOTED_STRATEGIES");
    refresh_files();
}

bool StrategyManagerPanel::is_static(const std::string& key) const {
    return static_keys_.count(key) != 0;
}

bool StrategyManagerPanel::info_for(const std::string& key,
                                    std::vector<StrategyHost::Param>& out) const {
    if (const tt::StaticStrategyEntry* e = tt::find_static_strategy(key)) {
        out.clear();
        for (uint32_t i = 0; i < e->info->param_count; ++i) {
            const ParamDesc& p = e->info->params[i];
            out.push_back({p.name ? p.name : "", p.def, p.min, p.max});
        }
        return true;
    }
    StrategyHost::ModuleView mv;
    if (!host_.info(key, mv)) return false;
    out = std::move(mv.params);
    return true;
}

StrategyManagerPanel::~StrategyManagerPanel() {
    if (build_thread_.joinable()) build_thread_.join();
}

void StrategyManagerPanel::adopt_params(const std::string& key) {
    std::vector<StrategyHost::Param> params;
    if (!info_for(key, params)) return;
    std::vector<ParamValue> fresh;
    fresh.reserve(params.size());
    const auto old = param_vals_.find(key);
    const auto saved = saved_params_.find(key);
    for (const auto& p : params) {
        double value = p.def;
        // Precedence: default < last-session saved value < current live edit
        // (so a first load restores saved values, a rebuild keeps live edits).
        if (saved != saved_params_.end()) {
            const auto it = saved->second.find(p.name);
            if (it != saved->second.end()) value = it->second;
        }
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
    if (build_sel_ >= static_cast<int>(files_.size())) build_sel_ = 0;
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

// Deferred work: queued build requests, then freshly built DLLs. Loading no
// longer needs the engine idle — replacing a module retires the old one, and
// its DLL lives until the last running instance is destroyed.
void StrategyManagerPanel::pump() {
    if (!load_queue_.empty() && !building_.load()) {
        const std::string want = load_queue_.front();
        load_queue_.pop_front();
        if (!loaded_fresh(want))
            start_build((fs::path(dir_) / want).string());
    }

    std::string dll, src;
    {
        std::lock_guard lock(pending_mu_);
        dll.swap(pending_dll_);
        src.swap(pending_src_);
    }
    if (dll.empty()) return;
    std::string err;
    if (host_.load(dll, src, err)) {
        const std::string key = fs::path(src).filename().string();
        adopt_params(key);
        console("loaded: " + display_name(key));
    } else {
        console("load failed: " + err);
    }
}

std::string StrategyManagerPanel::display_name(const std::string& key) const {
    if (key.empty()) return "SMA Crossover (built-in)";
    if (const tt::StaticStrategyEntry* e = tt::find_static_strategy(key))
        return e->info->name ? e->info->name : key;
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
    std::vector<StrategyHost::Param> params;   // loaded but never edited: defaults
    if (info_for(key, params))
        for (const auto& p : params) out[p.name] = p.def;
    return out;
}

std::vector<StrategyManagerPanel::ParamSpec> StrategyManagerPanel::param_specs(
    const std::string& key) const {
    std::vector<ParamSpec> out;
    const auto it = param_vals_.find(key);
    if (it != param_vals_.end()) {
        for (const auto& p : it->second) out.push_back({p.name, p.value, p.min, p.max});
        return out;
    }
    std::vector<StrategyHost::Param> params;   // loaded but never edited: declared defaults
    if (info_for(key, params))
        for (const auto& p : params) out.push_back({p.name, p.def, p.min, p.max});
    return out;
}

std::vector<std::string> StrategyManagerPanel::loaded_keys() const {
    std::vector<std::string> out;
    for (const auto& e : tt::static_strategy_registry()) out.push_back(e.key);
    for (const auto& m : host_.modules())
        if (!is_static(m.key)) out.push_back(m.key);   // a rebuilt-but-inert DLL: hidden
    return out;
}

std::map<std::string, std::map<std::string, double>>
StrategyManagerPanel::all_param_values() const {
    std::map<std::string, std::map<std::string, double>> out;
    out[""] = param_values("");
    for (const auto& e : tt::static_strategy_registry()) out[e.key] = param_values(e.key);
    for (const auto& m : host_.modules())
        if (!is_static(m.key)) out[m.key] = param_values(m.key);
    return out;
}

void StrategyManagerPanel::restore_state(
    const std::vector<std::string>& loaded,
    const std::map<std::string, std::map<std::string, double>>& params) {
    saved_params_ = params;
    // Apply saved values to the built-in (already seeded with descriptors).
    const auto b = saved_params_.find("");
    if (b != saved_params_.end())
        for (auto& p : param_vals_[""]) {
            const auto it = b->second.find(p.name);
            if (it != b->second.end()) p.value = it->second;
        }
    // Promoted strategies have declared params available immediately (no
    // build needed, unlike "" they aren't pre-seeded in the ctor) -- seed them
    // now so saved values apply without waiting for the panel to touch this
    // key first (which is what the "" branch above and adopt_params do for
    // the built-in and for a freshly-(re)loaded DLL respectively).
    for (const std::string& key : static_keys_) adopt_params(key);
    // Rebuild + hot-load each saved DLL strategy; adopt_params applies its
    // saved params once the module is up. No-ops for "" and promoted keys
    // (loaded_fresh is already true for both).
    for (const std::string& key : loaded)
        if (!key.empty()) request_load(key);
}

void StrategyManagerPanel::set_param_values(const std::string& key,
                                            const std::map<std::string, double>& values) {
    std::vector<ParamValue>* ps = editor_params(key);
    if (!ps) return;
    for (auto& p : *ps) {
        const auto it = values.find(p.name);
        if (it == values.end()) continue;
        p.value = it->second;
        if (p.value < p.min) p.value = p.min;
        if (p.value > p.max) p.value = p.max;
    }
}

bool StrategyManagerPanel::loaded_fresh(const std::string& key) const {
    if (key.empty() || is_static(key)) return true;   // built-in / promoted: nothing to build
    return host_.has(key) && !host_.stale(key);
}

void StrategyManagerPanel::request_load(const std::string& key) {
    if (key.empty() || loaded_fresh(key)) return;
    // Queue it (deduped); pump() builds queued strategies one at a time, so a
    // batch (e.g. restoring last session's strategies) all get built.
    for (const std::string& q : load_queue_)
        if (q == key) return;
    load_queue_.push_back(key);
}

bool StrategyManagerPanel::load_pending() const {
    if (building_.load() || !load_queue_.empty()) return true;
    std::lock_guard lock(pending_mu_);
    return !pending_dll_.empty();
}

std::vector<StrategyManagerPanel::ParamValue>* StrategyManagerPanel::editor_params(
    const std::string& key) {
    const auto it = param_vals_.find(key);
    if (it != param_vals_.end()) return &it->second;
    if (!key.empty() && (is_static(key) || host_.has(key))) {
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

    // ---- build/load a source (a loaded strategy gets its own tab below) ----
    ImGui::TextUnformatted("Build a strategy:");
    ImGui::SameLine();
    if (files_.empty()) {
        ImGui::TextDisabled("(no .cpp files in strategies/)");
    } else {
        if (build_sel_ >= static_cast<int>(files_.size())) build_sel_ = 0;
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##buildsrc",
                              files_[static_cast<size_t>(build_sel_)].c_str())) {
            for (int i = 0; i < static_cast<int>(files_.size()); ++i)
                if (ImGui::Selectable(files_[static_cast<size_t>(i)].c_str(),
                                      i == build_sel_))
                    build_sel_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(building_.load());
        if (ImGui::Button(building_.load() ? "Building..." : "Build & Load"))
            start_build(
                (fs::path(dir_) / files_[static_cast<size_t>(build_sel_)]).string());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Compile + hot-load into a tab. Compiler output is in "
                              "the Build Output window (View menu).");
    }
    ImGui::Separator();

    // ---- one tab per strategy: built-in + each loaded module ----
    if (ImGui::BeginTabBar("##strat_tabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                               ImGuiTabBarFlags_Reorderable |
                                               ImGuiTabBarFlags_FittingPolicyScroll |
                                               ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        const std::vector<StrategyHost::ModuleView> mods = host_.modules();
        // Tab-list button only when the tabs overflow (would have shrunk).
        if (const ImGuiTabBar* tb = ImGui::GetCurrentTabBar();
            tb && tb->WidthAllTabsIdeal > tb->BarRect.GetWidth() + 1.0f) {
            if (ImGui::TabItemButton("  ##stratlist", ImGuiTabItemFlags_Leading |
                                                          ImGuiTabItemFlags_NoTooltip))
                ImGui::OpenPopup("##stratlist");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
            const float rr = ImGui::GetFontSize() * 0.26f;
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddTriangleFilled(ImVec2(cx - rr, cy - rr * 0.5f),
                                  ImVec2(cx + rr, cy - rr * 0.5f),
                                  ImVec2(cx, cy + rr * 0.7f), col);
        }
        const ImGuiTabItemFlags bsel =
            want_tab_set_ && want_tab_.empty() ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("SMA (built-in)###builtin", nullptr, bsel)) {
            draw_strategy_tab("", nullptr);
            ImGui::EndTabItem();
        }
        // Promoted strategies: compiled in, nothing to build/unload -- same
        // shape as the built-in's tab (mod = nullptr), no close button.
        for (const auto& e : tt::static_strategy_registry()) {
            const std::string name = e.info->name ? e.info->name : e.key;
            const std::string label = name + "###" + e.key;
            const ImGuiTabItemFlags msel =
                want_tab_set_ && want_tab_ == e.key ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, msel)) {
                draw_strategy_tab(e.key, nullptr);
                ImGui::EndTabItem();
            }
        }
        for (const auto& m : mods) {
            // A promoted strategy's .cpp can still be rebuilt as a DLL from
            // the picker above, but acquire_strategy() always prefers the
            // static registry, so that module is inert -- no tab for it.
            if (is_static(m.key)) continue;
            const std::string label = m.name + "###" + m.key;   // stable id = key
            const ImGuiTabItemFlags msel =
                want_tab_set_ && want_tab_ == m.key ? ImGuiTabItemFlags_SetSelected : 0;
            // The tab's close button unloads the module — but only offer it
            // when no running session/backtest is using an instance.
            bool tab_open = true;
            if (ImGui::BeginTabItem(label.c_str(),
                                    m.instances > 0 ? nullptr : &tab_open, msel)) {
                draw_strategy_tab(m.key, &m);
                ImGui::EndTabItem();
            }
            if (!tab_open) {
                host_.unload(m.key);
                console("unloaded " + m.name);
            }
        }
        want_tab_set_ = false;   // consumed
        if (ImGui::BeginPopup("##stratlist")) {
            if (ImGui::Selectable("SMA Crossover (built-in)")) {
                want_tab_.clear();
                want_tab_set_ = true;
            }
            for (const auto& e : tt::static_strategy_registry()) {
                const std::string name = e.info->name ? e.info->name : e.key;
                if (ImGui::Selectable(name.c_str())) {
                    want_tab_ = e.key;
                    want_tab_set_ = true;
                }
            }
            for (const auto& m : mods)
                if (!is_static(m.key) && ImGui::Selectable(m.name.c_str())) {
                    want_tab_ = m.key;
                    want_tab_set_ = true;
                }
            ImGui::EndPopup();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void StrategyManagerPanel::draw_strategy_tab(const std::string& key,
                                             const StrategyHost::ModuleView* mod) {
    if (mod) {
        ImGui::TextDisabled("(%s)", mod->key.c_str());
        if (mod->instances > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("- %d in use", mod->instances);
        }
        if (host_.stale(key)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "- source changed");
        }
        ImGui::BeginDisabled(building_.load());
        if (ImGui::SmallButton(building_.load() ? "Building..." : "Rebuild"))
            start_build((fs::path(dir_) / key).string());
        ImGui::EndDisabled();
        ImGui::Separator();
    }

    // Per-strategy parameters.
    if (std::vector<ParamValue>* params = editor_params(key)) {
        if (params->empty()) {
            ImGui::TextDisabled("(no parameters)");
        } else {
            for (auto& p : *params) {
                ImGui::SetNextItemWidth(140);
                ImGui::DragScalar(p.name.c_str(), ImGuiDataType_Double, &p.value, 1.0f,
                                  &p.min, &p.max, "%.4g");
            }
            if (ImGui::SmallButton("Reset defaults"))
                for (auto& p : *params) p.value = p.def;
        }
    } else {
        ImGui::TextDisabled("Build & Load to edit this strategy's parameters.");
    }
}

void StrategyManagerPanel::draw_build_output(bool* open) {
    if (!ImGui::Begin("Build Output", open)) {
        ImGui::End();
        return;
    }
    tab_drag_hint();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard lock(out_mu_);
        output_.clear();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##ccout", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard lock(out_mu_);
        for (const auto& l : output_) {
            const bool is_err = l.find("error") != std::string::npos;
            if (is_err)
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1.0f), "%s", l.c_str());
            else
                ImGui::TextUnformatted(l.c_str());
        }
        // Follow new output only when already at the bottom; scrolling up to
        // read is never fought.
        if (output_.size() != out_seen_) {
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            out_seen_ = output_.size();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace tt::ui
