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
    case 0: return 0;   // 5m -> 1mo (gateway intraday history limit)
    case 1: return 2;   // 1h -> 1y
    default: return 5;
    }
}
} // namespace

void SweepPanel::restore(const Settings& s) {
    strat_key_ = s.strat_key;
    std::snprintf(sym_, sizeof sym_, "%s", s.symbol.c_str());
    interval_idx_ = std::clamp(s.interval_idx, 0, 2);
    range_idx_ = std::clamp(s.range_idx, 0, 5);
    cash_ = s.cash;
    metric_ = std::clamp(s.metric, 0, static_cast<int>(IM_ARRAYSIZE(kSweepMetrics)) - 1);
    use_holdout_ = s.holdout;
    holdout_pct_ = std::clamp(s.holdout_pct, 5.0, 50.0);
}

void SweepPanel::draw(bool* open, const std::vector<std::string>& strat_keys,
                      const NameFn& name, const ParamsFn& params_of, const State& st,
                      const RunFn& run, const TournamentFn& tournament,
                      const CancelFn& cancel) {
    const bool visible = ImGui::Begin("Optimizer", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    // Strategy to optimize (loaded modules only; build via the Strategy panel).
    // A pick that isn't loaded right now is kept — the startup restore rebuilds
    // strategies asynchronously.
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("strategy", name(strat_key_).c_str())) {
        if (ImGui::Selectable(name("").c_str(), strat_key_.empty())) strat_key_.clear();
        for (const std::string& k : strat_keys) {
            const std::string lbl = name(k) + "###" + k;
            if (ImGui::Selectable(lbl.c_str(), k == strat_key_)) strat_key_ = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Strategy to optimize (loaded strategies only — build via "
                          "the Strategy panel)");

    const int n_params = static_cast<int>(params_of(strat_key_).size());
    if (n_params == 0) {
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

    ImGui::SetNextItemWidth(130);
    ImGui::Combo("metric", &metric_, kSweepMetrics, IM_ARRAYSIZE(kSweepMetrics));
    ImGui::SameLine();
    ImGui::Checkbox("holdout", &use_holdout_);
    ImGui::SetItemTooltip("Optimize on the older data only, then score the winner "
                          "on the newest slice it never saw.\nIf the holdout number "
                          "collapses vs the in-sample best, the parameters are "
                          "overfit.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!use_holdout_);
    ImGui::SetNextItemWidth(60);
    ImGui::InputDouble("%##holdout", &holdout_pct_, 0, 0, "%.0f");
    ImGui::EndDisabled();
    holdout_pct_ = std::clamp(holdout_pct_, 5.0, 50.0);

    if (st.running || st.tourney.active) {
        ImGui::ProgressBar(st.total > 0 ? static_cast<float>(st.done) / st.total : 0.0f,
                           ImVec2(180, 0));
        ImGui::SameLine();
        if (st.tourney.active)
            ImGui::Text("candidate %d/%d — %s   (pass %d/%d — %s)", st.tourney.idx + 1,
                        st.tourney.total, st.tourney.current.c_str(), st.pass + 1,
                        st.n_passes, st.cur_param.c_str());
        else
            ImGui::Text("pass %d/%d — %s", st.pass + 1, st.n_passes, st.cur_param.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel") && cancel) cancel();
    } else {
        auto make_request = [&] {
            for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
            Request rq;
            rq.strat_key = strat_key_;
            rq.symbol = sym_;
            rq.interval = kIntervals[interval_idx_];
            rq.range = kRanges[range_idx_];
            rq.cash = cash_;
            rq.metric = metric_;
            rq.holdout_pct = use_holdout_ ? holdout_pct_ : 0.0;
            return rq;
        };
        ImGui::BeginDisabled(eng_.running() || !sym_[0]);
        char label[64];
        std::snprintf(label, sizeof label, "Optimize (%d backtests)",
                      kSweepPasses * n_params * kSweepSteps);
        if (ImGui::Button(label) && run) run(make_request());
        ImGui::SetItemTooltip("Coordinate descent over every declared parameter: each "
                              "pass sweeps each param across its range with the others "
                              "fixed at the best so far; pass 2 refines around the "
                              "winner. The best values are applied to the strategy.");
        ImGui::SameLine();
        if (ImGui::Button("Tournament") && tournament) tournament(make_request());
        ImGui::SetItemTooltip("Optimize EVERY loaded strategy (plus the built-in) on the "
                              "same data and crown the best holdout score; the champion "
                              "strategy + params are applied to this symbol's Trade tab.");
        ImGui::EndDisabled();
    }

    // ---- results ----
    if (st.has_best) {
        ImGui::Separator();
        std::string best;
        for (const auto& [k, v] : st.best) {
            char kv[64];
            std::snprintf(kv, sizeof kv, "%s%s=%.4g", best.empty() ? "" : "  ",
                          k.c_str(), v);
            best += kv;
        }
        ImGui::TextUnformatted(("best: " + best).c_str());
        ImGui::Text("%s %.4g (in-sample)", kSweepMetrics[st.metric], st.best_metric);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", st.label.c_str());
        if (st.has_holdout) {
            const bool collapsed = sweep_metric_minimize(st.metric)
                                       ? st.holdout_val > st.best_metric * 1.5
                                       : st.holdout_val < st.best_metric * 0.5;
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
        if (st.applied)
            ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1),
                               "applied to the strategy's parameters");
    }

    // Tournament ranking (once finished).
    if (st.tourney.done && !st.tourney.rows.empty()) {
        ImGui::SeparatorText(("Tournament — " + st.tourney.symbol).c_str());
        for (const auto& row : st.tourney.rows) {
            if (!row.valid) {
                ImGui::TextDisabled("%s — no result", row.name.c_str());
                continue;
            }
            if (row.champion)
                ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1),
                                   "%s   %s %.4g%s   <- champion, applied",
                                   row.name.c_str(), kSweepMetrics[st.metric], row.score,
                                   row.holdout ? " (holdout)" : "");
            else
                ImGui::Text("%s   %s %.4g%s", row.name.c_str(), kSweepMetrics[st.metric],
                            row.score, row.holdout ? " (holdout)" : "");
        }
    }

    // Live plot of the current (or last) 1-D param sweep.
    if (!st.xs.empty() &&
        ImPlot::BeginPlot("##sweepline", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(st.cur_param.c_str(), kSweepMetrics[st.metric],
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<double> ys;
        ys.reserve(st.vals.size());
        for (double v : st.vals) ys.push_back(std::isnan(v) ? 0.0 : v);
        ImPlot::SetNextLineStyle(ImVec4(0.35f, 0.65f, 0.95f, 1.0f), 2.0f);
        ImPlot::PlotLine("##m", st.xs.data(), ys.data(), static_cast<int>(st.xs.size()));
        ImPlot::EndPlot();
    }

    ImGui::End();
}

} // namespace tt::ui
