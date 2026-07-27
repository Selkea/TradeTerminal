#include "panels/chart.h"

#include "candle_plot.h"

#include "imgui.h"
#include "implot.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace tt::ui {

namespace {
constexpr const char* kIntervals[] = {"1s", "1m", "5m", "15m", "1h", "1d"};
constexpr double kIntervalSec[] = {1, 60, 300, 900, 3600, 86400};   // matches kIntervals
constexpr const char* kRanges[] = {"1m", "5m", "15m", "30m", "1h",
                                   "1d", "5d", "1mo", "6mo", "1y", "5y", "max"};

int range_idx_of(const char* r) {
    for (int i = 0; i < IM_ARRAYSIZE(kRanges); ++i)
        if (std::strcmp(kRanges[i], r) == 0) return i;
    return IM_ARRAYSIZE(kRanges) - 1;
}

// Providers limit intraday history; clamp the range so requests don't error.
// Keyed off the interval's seconds (and range names) so it survives reordering.
int max_range_idx(int interval_idx) {
    const double s = kIntervalSec[interval_idx];
    if (s <= 1) return range_idx_of("30m");    // 1s  -> IB caps ~30 min/req
    if (s <= 60) return range_idx_of("5d");    // 1m  -> up to 5d
    if (s <= 900) return range_idx_of("1mo");  // 5m/15m -> up to 1mo
    if (s <= 3600) return range_idx_of("1y");  // 1h  -> up to 1y
    return range_idx_of("max");                // 1d  -> anything
}
} // namespace

void ChartPanel::show_symbol(const std::string& symbol) {
    std::snprintf(sym_, sizeof(sym_), "%s", symbol.c_str());
    request();
}

void ChartPanel::restore(const std::string& sym, int ivl_idx, int rng_idx) {
    std::snprintf(sym_, sizeof(sym_), "%s", sym.c_str());
    interval_idx_ = std::clamp(ivl_idx, 0, static_cast<int>(IM_ARRAYSIZE(kIntervals)) - 1);
    range_idx_ = std::clamp(rng_idx, 0, static_cast<int>(IM_ARRAYSIZE(kRanges)) - 1);
    // No request here: the first connection-generation bump fires it.
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
    // The live tail restarts from the fresh historical data every rebuild.
    hist_last_ts_ = n ? xs_[n - 1] : 0.0;
    live_tail_bucket_ = 0.0;
    // Follow-window width = the span the fetch actually covers.
    view_span_ = n > 1 ? xs_[n - 1] - xs_[0] : 0.0;
}

void ChartPanel::draw(bool* open, const std::vector<FillMarker>& fills,
                      double live_price, double live_ts_sec) {
    const bool visible = ImGui::Begin("Chart", open);
    tab_drag_hint();
    if (!visible) {
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
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_live_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll the view to keep the latest live bar in sight");
    if (from_cache_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(cache)");
    }

    // Pull in fresh data if the store advanced.
    SeriesStore::Series series;
    if (store_.copy_if_newer(sym_, kIntervals[interval_idx_], seen_rev_, series))
        rebuild_plot_arrays(series);

    // Live tail (approach B): refine the newest bar, or roll a new one, from the
    // session's latest price — real-time updates without a historical re-fetch.
    // Bucketing off the tick time means a stale feed simply stops extending it.
    // Intraday only (daily-bar timestamps aren't reliably interval-aligned).
    const bool live_active = live_price > 0.0 && live_ts_sec > 0.0 &&
                             kIntervalSec[interval_idx_] < 86400.0 && !xs_.empty();
    if (live_active) {
        const double ivl = kIntervalSec[interval_idx_];
        const double bucket = std::floor(live_ts_sec / ivl) * ivl;
        auto refine_back = [&] {
            highs_.back() = std::max(highs_.back(), live_price);
            lows_.back() = std::min(lows_.back(), live_price);
            closes_.back() = live_price;
        };
        if (bucket == hist_last_ts_) {
            refine_back();   // update the current (partially-fetched) bar
        } else if (bucket > hist_last_ts_) {
            if (bucket == live_tail_bucket_) {
                refine_back();   // same live bar, still forming
            } else {             // a new interval started: append a fresh bar
                xs_.push_back(bucket);
                opens_.push_back(live_price);
                highs_.push_back(live_price);
                lows_.push_back(live_price);
                closes_.push_back(live_price);
                vols_.push_back(0.0);
                live_tail_bucket_ = bucket;
            }
        }
    }

    const int n = static_cast<int>(xs_.size());
    // Follow mode: after the initial fit, slide the X view each frame so the
    // newest live bar stays on screen (otherwise the live tail scrolls off the
    // right edge and the chart looks frozen). Y auto-fits to the framed data.
    const bool follow = follow_live_ && live_active && view_span_ > 0.0 && n > 0;
    const double x_hi = n > 0 ? xs_.back() : 0.0;
    const auto follow_x = [&] {
        if (follow)
            ImPlot::SetupAxisLimits(ImAxis_X1, x_hi - view_span_, x_hi + width_sec_,
                                    ImPlotCond_Always);
    };
    const ImPlotAxisFlags y_flags =
        ImPlotAxisFlags_Opposite | (follow ? ImPlotAxisFlags_AutoFit : 0);
    static float ratios[] = {3.0f, 1.0f};
    if (ImPlot::BeginSubplots("##ohlcv", 2, 1, ImVec2(-1, -1),
                              ImPlotSubplotFlags_LinkCols, ratios)) {
        if (fit_next_ && !follow) ImPlot::SetNextAxesToFit();  // follow owns the view while live
        if (ImPlot::BeginPlot("##price", ImVec2(), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxes(nullptr, "price", ImPlotAxisFlags_NoLabel, y_flags);
            follow_x();
            if (n > 0)
                PlotCandlestick(sym_, xs_.data(), opens_.data(), highs_.data(),
                                lows_.data(), closes_.data(), n, width_sec_);
            if (!fills.empty()) {
                // Session/backtest fills on top of the candles.
                std::vector<double> bx, by, sx, sy;
                for (const FillMarker& f : fills) {
                    (f.buy ? bx : sx).push_back(f.ts_sec);
                    (f.buy ? by : sy).push_back(f.price);
                }
                if (!bx.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Up, 6.0f,
                                               ImVec4(0.2f, 0.9f, 0.4f, 1.0f), 1.0f,
                                               ImVec4(0.05f, 0.35f, 0.15f, 1.0f));
                    ImPlot::PlotScatter("##buys", bx.data(), by.data(),
                                        static_cast<int>(bx.size()));
                }
                if (!sx.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Down, 6.0f,
                                               ImVec4(0.95f, 0.35f, 0.3f, 1.0f), 1.0f,
                                               ImVec4(0.4f, 0.1f, 0.08f, 1.0f));
                    ImPlot::PlotScatter("##sells", sx.data(), sy.data(),
                                        static_cast<int>(sx.size()));
                }
            }
            ImPlot::EndPlot();
        }
        if (fit_next_ && !follow) ImPlot::SetNextAxesToFit();  // follow owns the view while live
        if (ImPlot::BeginPlot("##volume", ImVec2(), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxes(nullptr, "vol", ImPlotAxisFlags_NoLabel, y_flags);
            follow_x();
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
