#include "panels/sweep.h"

#include "imgui.h"
#include "implot.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace tt::ui {

namespace {
constexpr const char* kIntervals[] = {"5m", "1h", "1d"};
constexpr const char* kRanges[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};

int max_range_idx(int interval_idx) {
    switch (interval_idx) {
    case 0: return 0;   // 5m -> 1mo (Yahoo intraday history limit)
    case 1: return 2;   // 1h -> 1y
    default: return 5;
    }
}

// Combo over parameter names; returns selected index into names.
int param_combo(const char* id, const std::vector<std::string>& names, int idx,
                bool with_none) {
    const int base = with_none ? 1 : 0;
    const char* preview = idx == 0 && with_none
                              ? "(none)"
                              : names[static_cast<size_t>(idx - base)].c_str();
    if (ImGui::BeginCombo(id, preview)) {
        if (with_none && ImGui::Selectable("(none)", idx == 0)) idx = 0;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (ImGui::Selectable(names[static_cast<size_t>(i)].c_str(), idx == i + base))
                idx = i + base;
        ImGui::EndCombo();
    }
    return idx;
}
} // namespace

void SweepPanel::draw(bool* open, const std::vector<std::string>& strat_keys,
                      const NameFn& name, const ParamsFn& params_of, const State& st,
                      const RunFn& run, const CancelFn& cancel) {
    const bool visible = ImGui::Begin("Optimizer", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    // Strategy to sweep (loaded modules only; build via the Strategy panel).
    if (!strat_key_.empty() &&
        std::find(strat_keys.begin(), strat_keys.end(), strat_key_) == strat_keys.end())
        strat_key_.clear();   // picked module was unloaded: fall back to built-in
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("strategy", name(strat_key_).c_str())) {
        if (ImGui::Selectable(name("").c_str(), strat_key_.empty())) strat_key_.clear();
        for (const std::string& k : strat_keys) {
            const std::string lbl = name(k) + "###" + k;
            if (ImGui::Selectable(lbl.c_str(), k == strat_key_)) strat_key_ = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Strategy the grid sweeps (loaded strategies only — build "
                          "via the Strategy panel)");

    const std::map<std::string, double> params = params_of(strat_key_);
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto& [k, v] : params) names.push_back(k);

    if (names.empty()) {
        ImGui::TextDisabled("%s exposes no parameters.", name(strat_key_).c_str());
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##swsym", sym_, sizeof(sym_), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::Combo("##swivl", &interval_idx_, kIntervals, IM_ARRAYSIZE(kIntervals));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::Combo("##swrng", &range_idx_, kRanges, IM_ARRAYSIZE(kRanges));
    range_idx_ = std::min(range_idx_, max_range_idx(interval_idx_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");

    px_idx_ = std::clamp(px_idx_, 0, static_cast<int>(names.size()) - 1);
    py_idx_ = std::clamp(py_idx_, 0, static_cast<int>(names.size()));

    ImGui::SetNextItemWidth(110);
    px_idx_ = param_combo("param X", names, px_idx_, false);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    double xr[2] = {x0_, x1_};
    ImGui::InputScalarN("range##x", ImGuiDataType_Double, xr, 2);
    x0_ = xr[0]; x1_ = xr[1];
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("steps##x", &nx_);
    nx_ = std::clamp(nx_, 2, 200);

    ImGui::SetNextItemWidth(110);
    py_idx_ = param_combo("param Y", names, py_idx_, true);
    const bool has_y = py_idx_ > 0;
    ImGui::BeginDisabled(!has_y);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    double yr[2] = {y0_, y1_};
    ImGui::InputScalarN("range##y", ImGuiDataType_Double, yr, 2);
    y0_ = yr[0]; y1_ = yr[1];
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("steps##y", &ny_);
    ny_ = std::clamp(ny_, 2, 200);
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(130);
    ImGui::Combo("metric", &metric_, kSweepMetrics, IM_ARRAYSIZE(kSweepMetrics));
    ImGui::SameLine();
    ImGui::Checkbox("holdout", &use_holdout_);
    ImGui::SetItemTooltip("Optimize on the older data only, then score the best "
                          "cell on the newest slice it never saw.\nIf the holdout "
                          "number collapses vs the grid's best, the parameters are "
                          "overfit.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!use_holdout_);
    ImGui::SetNextItemWidth(60);
    ImGui::InputDouble("%##holdout", &holdout_pct_, 0, 0, "%.0f");
    ImGui::EndDisabled();
    holdout_pct_ = std::clamp(holdout_pct_, 5.0, 50.0);

    const int total = nx_ * (has_y ? ny_ : 1);
    if (st.running) {
        ImGui::ProgressBar(st.total > 0 ? static_cast<float>(st.done) / st.total : 0.0f,
                           ImVec2(180, 0));
        ImGui::SameLine();
        ImGui::Text("%d / %d", st.done, st.total);
        ImGui::SameLine();
        if (ImGui::Button("Cancel") && cancel) cancel();
    } else {
        ImGui::BeginDisabled(eng_.running() || !sym_[0]);
        char label[48];
        std::snprintf(label, sizeof label, "Run sweep (%d backtests)", total);
        if (ImGui::Button(label) && run) {
            for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
            Request rq;
            rq.strat_key = strat_key_;
            rq.symbol = sym_;
            rq.interval = kIntervals[interval_idx_];
            rq.range = kRanges[range_idx_];
            rq.cash = cash_;
            rq.px = names[static_cast<size_t>(px_idx_)];
            rq.x0 = x0_; rq.x1 = x1_; rq.nx = nx_;
            if (has_y) {
                rq.py = names[static_cast<size_t>(py_idx_ - 1)];
                rq.y0 = y0_; rq.y1 = y1_; rq.ny = ny_;
            }
            rq.metric = metric_;
            rq.holdout_pct = use_holdout_ ? holdout_pct_ : 0.0;
            run(rq);
        }
        ImGui::EndDisabled();
    }
    // ---- results ----
    if (st.total == 0 || st.vals.empty()) {
        ImGui::End();
        return;
    }
    ImGui::Separator();

    // Best cell so far (ignores unfinished NaN cells).
    int best = -1;
    for (int i = 0; i < static_cast<int>(st.vals.size()); ++i) {
        if (std::isnan(st.vals[static_cast<size_t>(i)])) continue;
        if (best < 0) { best = i; continue; }
        const double a = st.vals[static_cast<size_t>(i)], b = st.vals[static_cast<size_t>(best)];
        if (sweep_metric_minimize(st.metric) ? a < b : a > b) best = i;
    }
    if (best >= 0) {
        const int nx = static_cast<int>(st.xs.size());
        const int ix = best % nx, iy = best / nx;
        char buf[160];
        if (!st.py.empty())
            std::snprintf(buf, sizeof buf, "best: %s=%.4g  %s=%.4g  ->  %s %.4g",
                          st.px.c_str(), st.xs[static_cast<size_t>(ix)], st.py.c_str(),
                          st.ys[static_cast<size_t>(iy)], kSweepMetrics[st.metric],
                          st.vals[static_cast<size_t>(best)]);
        else
            std::snprintf(buf, sizeof buf, "best: %s=%.4g  ->  %s %.4g", st.px.c_str(),
                          st.xs[static_cast<size_t>(ix)], kSweepMetrics[st.metric],
                          st.vals[static_cast<size_t>(best)]);
        ImGui::TextUnformatted(buf);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", st.label.c_str());
        if (st.has_holdout) {
            const double train = st.vals[static_cast<size_t>(best)];
            const bool collapsed = sweep_metric_minimize(st.metric)
                                       ? st.holdout_val > train * 1.5
                                       : st.holdout_val < train * 0.5;
            ImGui::Text("holdout (last %.0f%%, unseen): %s %.4g", st.holdout_pct,
                        kSweepMetrics[st.metric], st.holdout_val);
            if (collapsed) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1),
                                   "— much worse than in-sample: likely overfit");
            }
        } else if (st.holdout_pct > 0 && st.running) {
            ImGui::TextDisabled("holdout run pending...");
        }
    }

    const int nx = static_cast<int>(st.xs.size());
    const int ny = static_cast<int>(st.ys.size());
    if (ny > 0) {
        // Heatmap wants row 0 at the TOP (max y): flip rows, and map NaN
        // (pending cells) to the running minimum so they read as "cold".
        double vmin = 1e300, vmax = -1e300;
        for (double v : st.vals)
            if (!std::isnan(v)) {
                vmin = std::min(vmin, v);
                vmax = std::max(vmax, v);
            }
        if (vmin > vmax) { vmin = 0; vmax = 1; }
        std::vector<double> flipped(st.vals.size(), vmin);
        for (int iy = 0; iy < ny; ++iy)
            for (int ix = 0; ix < nx; ++ix) {
                const double v = st.vals[static_cast<size_t>(iy * nx + ix)];
                flipped[static_cast<size_t>((ny - 1 - iy) * nx + ix)] =
                    std::isnan(v) ? vmin : v;
            }
        ImPlot::PushColormap(ImPlotColormap_Viridis);
        if (ImPlot::BeginPlot("##sweepmap", ImVec2(-1, -1))) {
            ImPlot::SetupAxes(st.px.c_str(), st.py.c_str());
            const double hx = nx > 1 ? (st.xs.back() - st.xs.front()) / (nx - 1) / 2 : 0.5;
            const double hy = ny > 1 ? (st.ys.back() - st.ys.front()) / (ny - 1) / 2 : 0.5;
            ImPlot::PlotHeatmap("##vals", flipped.data(), ny, nx, vmin, vmax,
                                nx * ny <= 64 ? "%.2f" : nullptr,
                                ImPlotPoint(st.xs.front() - hx, st.ys.front() - hy),
                                ImPlotPoint(st.xs.back() + hx, st.ys.back() + hy));
            ImPlot::EndPlot();
        }
        ImPlot::PopColormap();
    } else if (ImPlot::BeginPlot("##sweepline", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(st.px.c_str(), kSweepMetrics[st.metric],
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<double> ys;
        ys.reserve(st.vals.size());
        for (double v : st.vals) ys.push_back(std::isnan(v) ? 0.0 : v);
        ImPlot::SetNextLineStyle(ImVec4(0.35f, 0.65f, 0.95f, 1.0f), 2.0f);
        ImPlot::PlotLine("##m", st.xs.data(), ys.data(), nx);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

} // namespace tt::ui
