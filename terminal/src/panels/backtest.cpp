#include "panels/backtest.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cctype>

namespace tt::ui {

namespace {
constexpr const char* kIntervals[] = {"5m", "1h", "1d"};
constexpr const char* kRanges[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};

int max_range_idx(int interval_idx) {
    switch (interval_idx) {
    case 0: return 0;   // 5m -> 1mo (Yahoo intraday history limit)
    case 1: return 2;   // 1h -> 1y
    default: return 5;  // 1d -> anything
    }
}
} // namespace

void BacktestPanel::draw(bool* open, const std::string& strategy_name, const RunFn& run) {
    if (!ImGui::Begin("Backtest", open)) {
        ImGui::End();
        return;
    }

    BacktestResult r;
    if (eng_.take_result(r)) {
        res_ = std::move(r);
        has_res_ = true;
    }

    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##btsym", sym_, sizeof(sym_), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::Combo("##btivl", &interval_idx_, kIntervals, IM_ARRAYSIZE(kIntervals));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::Combo("##btrng", &range_idx_, kRanges, IM_ARRAYSIZE(kRanges));
    range_idx_ = std::min(range_idx_, max_range_idx(interval_idx_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");

    const bool busy = eng_.running();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? "Running..." : "Run backtest") && sym_[0] && run) {
        for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
        run(sym_, kIntervals[interval_idx_], kRanges[range_idx_], cash_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", strategy_name.c_str());

    if (has_res_) {
        ImGui::Separator();
        draw_results();
    }
    ImGui::End();
}

void BacktestPanel::draw_results() {
    ImGui::Text("%s — %d fills, %llu events in %.1f ms", res_.symbol.c_str(), res_.trades,
                static_cast<unsigned long long>(res_.events), res_.duration_ms);

    if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_BordersInnerV)) {
        auto row = [](const char* k1, const std::string& v1,
                      const char* k2, const std::string& v2) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k1);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v1.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k2);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v2.c_str());
        };
        char a[64], b[64];
        std::snprintf(a, sizeof(a), "%+.2f%%", res_.total_return * 100.0);
        std::snprintf(b, sizeof(b), "%.2f", res_.final_equity);
        row("Return", a, "Final equity", b);
        std::snprintf(a, sizeof(a), "%.2f%%", res_.max_drawdown * 100.0);
        std::snprintf(b, sizeof(b), "%.2f", res_.sharpe);
        row("Max drawdown", a, "Sharpe", b);
        std::snprintf(a, sizeof(a), "%d / %d", res_.wins, res_.losses);
        std::snprintf(b, sizeof(b), "%.0f%%", res_.win_rate * 100.0);
        row("Wins / losses", a, "Win rate", b);
        std::snprintf(a, sizeof(a), "%.1f / %.1f us",
                      res_.lat_p50 / 1000.0, res_.lat_p99 / 1000.0);
        std::snprintf(b, sizeof(b), "%.1f us (%llu samples)", res_.lat_max / 1000.0,
                      static_cast<unsigned long long>(res_.lat_count));
        row("Tick->order p50/p99", a, "Max", b);
        ImGui::EndTable();
    }

    if (!res_.eq_ts.empty() &&
        ImPlot::BeginPlot("Equity", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxes(nullptr, "equity", ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
        ImPlot::SetNextLineStyle(ImVec4(0.35f, 0.65f, 0.95f, 1.0f), 2.0f);
        ImPlot::PlotLine("##eq", res_.eq_ts.data(), res_.eq_val.data(),
                         static_cast<int>(res_.eq_ts.size()));
        ImPlot::EndPlot();
    }
}

} // namespace tt::ui
