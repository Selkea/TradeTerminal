#include "panels/backtest.h"

#include "imgui.h"
#include "implot.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace tt::ui {

namespace {
constexpr const char* kIntervals[] = {"1m", "2m", "5m", "15m", "30m", "1h", "1d"};
constexpr const char* kRanges[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};

int max_range_idx(int interval_idx) {
    // Gateway history depth shrinks for finer bars; coarser bars go back further.
    // kIntervals = {1m, 2m, 5m, 15m, 30m, 1h, 1d}
    if (interval_idx <= 4) return 0;   // intraday (<=30m) -> 1mo
    if (interval_idx == 5) return 2;   // 1h -> 1y
    return 5;                          // 1d -> max
}
} // namespace

void BacktestPanel::set_symbol(const std::string& sym) {
    std::snprintf(sym_, sizeof(sym_), "%s", sym.c_str());
}

void BacktestPanel::scan_replay_files() {
    replay_files_.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(sessions_dir_, ec))
        if (e.is_regular_file() && e.path().extension() == ".ttk")
            replay_files_.push_back(e.path().filename().string());
    std::sort(replay_files_.rbegin(), replay_files_.rend());   // newest first
    replay_idx_ = 0;
}

void BacktestPanel::draw(bool* open, const std::vector<std::string>& sources,
                         const std::string& active_key,
                         const std::function<bool(const std::string&)>& loaded_fresh,
                         bool activating, bool suppress_result, const RunFn& run,
                         const ReplayFn& replay) {
    const bool visible = ImGui::Begin("Backtest", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    BacktestResult r;
    if (!suppress_result && eng_.take_result(r)) {
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

    // Strategy dropdown: Run builds + loads the pick if it isn't active yet.
    if (!strat_init_) {
        strat_sel_ = active_key;
        strat_init_ = true;
    }
    constexpr const char* kBuiltin = "SMA Crossover (built-in)";
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##btstrat",
                          strat_sel_.empty() ? kBuiltin : strat_sel_.c_str())) {
        if (ImGui::Selectable(kBuiltin, strat_sel_.empty())) strat_sel_.clear();
        for (const std::string& s : sources)
            if (ImGui::Selectable(s.c_str(), s == strat_sel_)) strat_sel_ = s;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Strategy for the next run; params are edited in "
                          "the Strategy panel");
    ImGui::SameLine();

    const bool busy = eng_.running() || activating;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(activating ? "Building..."
                                 : (busy ? "Running..." : "Run backtest")) &&
        sym_[0] && run) {
        for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
        run(strat_sel_, sym_, kIntervals[interval_idx_], kRanges[range_idx_], cash_);
    }
    ImGui::EndDisabled();
    if (!activating && loaded_fresh && !loaded_fresh(strat_sel_)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(builds on Run)");
    }

    // ---- session replay: re-run a captured .ttk session through the strategy ----
    ImGui::Separator();
    ImGui::TextDisabled("Session replay");
    if (!replay_scanned_) {
        scan_replay_files();
        replay_scanned_ = true;
    }
    if (replay_files_.empty()) {
        ImGui::TextDisabled("(no .ttk files yet — record a session in the Trade panel)");
    } else {
        if (replay_idx_ >= static_cast<int>(replay_files_.size())) replay_idx_ = 0;
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##replay_file", replay_files_[replay_idx_].c_str())) {
            for (int i = 0; i < static_cast<int>(replay_files_.size()); ++i)
                if (ImGui::Selectable(replay_files_[i].c_str(), i == replay_idx_))
                    replay_idx_ = i;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("bar sec##replay", &replay_bar_sec_, 0, 0);
        replay_bar_sec_ = std::max(0, replay_bar_sec_);
        ImGui::SetItemTooltip("Re-bar the recording at this many seconds for on_bar "
                              "strategies. 0 = use the size it was recorded with.");
        ImGui::SameLine();
        ImGui::BeginDisabled(eng_.running());
        if (ImGui::Button("Replay") && replay)
            replay(sessions_dir_ + "\\" + replay_files_[replay_idx_], replay_bar_sec_);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Re-run these captured ticks through the current strategy "
                              "+ fill simulator");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh")) scan_replay_files();

    if (has_res_) {
        ImGui::Separator();
        draw_results();
    }
    ImGui::End();
}

void BacktestPanel::export_csv() {
    const char* base = std::getenv("LOCALAPPDATA");
    const auto dir = std::filesystem::path(base ? base : ".") / "TradeTerminal" / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / ("backtest_" + res_.symbol + "_" +
                             std::to_string(std::time(nullptr)) + ".csv");
    std::ofstream f(path);
    if (!f) return;
    f << "# symbol," << res_.symbol << "\n# return," << res_.total_return
      << "\n# final_equity," << res_.final_equity << "\n# max_drawdown," << res_.max_drawdown
      << "\n# sharpe," << res_.sharpe << "\n# trades," << res_.trades
      << "\n# win_rate," << res_.win_rate << "\n# tick_to_order_p50_ns," << res_.lat_p50
      << "\n# tick_to_order_p99_ns," << res_.lat_p99 << "\n# tick_to_order_max_ns,"
      << res_.lat_max << "\n";
    f << "section,ts,side,qty,price,fee\n";
    for (const TradeRow& t : res_.fills)
        f << "fill," << t.ts_ns / 1'000'000'000 << ","
          << (t.side == static_cast<uint8_t>(Side::Buy) ? "buy" : "sell") << "," << t.qty
          << "," << t.price << "," << t.fee << "\n";
    f << "section,ts,equity,,,\n";
    for (size_t i = 0; i < res_.eq_ts.size(); ++i)
        f << "equity," << static_cast<int64_t>(res_.eq_ts[i]) << "," << res_.eq_val[i]
          << ",,,\n";
    last_export_ = path.string();
}

void BacktestPanel::draw_results() {
    ImGui::Text("%s — %d fills, %llu events in %.1f ms", res_.symbol.c_str(), res_.trades,
                static_cast<unsigned long long>(res_.events), res_.duration_ms);
    ImGui::SameLine();
    if (ImGui::SmallButton("Export CSV")) export_csv();
    if (!last_export_.empty()) ImGui::TextDisabled("saved: %s", last_export_.c_str());

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
