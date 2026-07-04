#include "panels/chart.h"

#include "candle_plot.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace tt::ui {

namespace {
constexpr const char* kIntervals[] = {"1m", "5m", "15m", "1h", "1d"};
constexpr const char* kRanges[] = {"1d", "5d", "1mo", "6mo", "1y", "5y", "max"};

// Yahoo limits intraday history; clamp the range so requests don't 4xx.
// (1m ≈ last 7 days, other intraday ≈ 60 days, 1h ≈ 2 years.)
int max_range_idx(int interval_idx) {
    switch (interval_idx) {
    case 0: return 1;   // 1m  -> up to 5d
    case 1:             // 5m
    case 2: return 2;   // 15m -> up to 1mo
    case 3: return 4;   // 1h  -> up to 1y
    default: return 6;  // 1d  -> anything
    }
}
} // namespace

void ChartPanel::show_symbol(const std::string& symbol) {
    std::snprintf(sym_, sizeof(sym_), "%s", symbol.c_str());
    request();
}

void ChartPanel::request() {
    range_idx_ = std::min(range_idx_, max_range_idx(interval_idx_));
    for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
    if (sym_[0] && ipc_.request_candles(sym_, kIntervals[interval_idx_], kRanges[range_idx_]))
        requested_once_ = true;
}

void ChartPanel::rebuild_plot_arrays(const SeriesStore::Series& s) {
    const size_t n = s.candles.size();
    xs_.resize(n); opens_.resize(n); highs_.resize(n);
    lows_.resize(n); closes_.resize(n); vols_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Candle& c = s.candles[i];
        xs_[i] = static_cast<double>(c.ts);
        opens_[i] = c.open; highs_[i] = c.high;
        lows_[i] = c.low; closes_[i] = c.close; vols_[i] = c.volume;
    }
    // Body width from the median inter-candle gap (robust to session breaks).
    width_sec_ = 60.0;
    if (n > 2) {
        std::vector<double> gaps;
        gaps.reserve(n - 1);
        for (size_t i = 1; i < n; ++i) gaps.push_back(xs_[i] - xs_[i - 1]);
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        width_sec_ = gaps[gaps.size() / 2] * 0.7;
    }
    from_cache_ = s.cached;
    fit_next_ = true;
}

void ChartPanel::draw(bool* open) {
    if (!ImGui::Begin("Chart", open)) {
        ImGui::End();
        return;
    }

    // First request once the feed comes up; re-request on reconnect.
    const uint64_t gen = ipc_.connection_generation();
    if (gen != seen_conn_gen_ && ipc_.connected()) {
        seen_conn_gen_ = gen;
        request();
    }

    ImGui::SetNextItemWidth(90);
    if (ImGui::InputText("##sym", sym_, sizeof(sym_),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                         ImGuiInputTextFlags_CharsUppercase)) request();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::Combo("##ivl", &interval_idx_, kIntervals, IM_ARRAYSIZE(kIntervals))) request();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::Combo("##rng", &range_idx_, kRanges, IM_ARRAYSIZE(kRanges))) request();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) request();
    if (from_cache_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(cache)");
    }
    if (!ipc_.connected()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "feed down");
    }

    // Pull in fresh data if the store advanced.
    SeriesStore::Series series;
    if (store_.copy_if_newer(sym_, kIntervals[interval_idx_], seen_rev_, series))
        rebuild_plot_arrays(series);

    const int n = static_cast<int>(xs_.size());
    static float ratios[] = {3.0f, 1.0f};
    if (ImPlot::BeginSubplots("##ohlcv", 2, 1, ImVec2(-1, -1),
                              ImPlotSubplotFlags_LinkCols, ratios)) {
        if (fit_next_) ImPlot::SetNextAxesToFit();
        if (ImPlot::BeginPlot("##price", ImVec2(), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxes(nullptr, "price",
                              ImPlotAxisFlags_NoLabel, ImPlotAxisFlags_Opposite);
            if (n > 0)
                PlotCandlestick(sym_, xs_.data(), opens_.data(), highs_.data(),
                                lows_.data(), closes_.data(), n, width_sec_);
            ImPlot::EndPlot();
        }
        if (fit_next_) ImPlot::SetNextAxesToFit();
        if (ImPlot::BeginPlot("##volume", ImVec2(), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxes(nullptr, "vol",
                              ImPlotAxisFlags_NoLabel, ImPlotAxisFlags_Opposite);
            if (n > 0) {
                ImPlot::SetNextFillStyle(ImVec4(0.35f, 0.55f, 0.85f, 0.6f));
                ImPlot::PlotBars("##vol", xs_.data(), vols_.data(), n, width_sec_);
            }
            ImPlot::EndPlot();
        }
        fit_next_ = false;
        ImPlot::EndSubplots();
    }
    ImGui::End();
}

} // namespace tt::ui
