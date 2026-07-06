#include "panels/trade.h"

#include "imgui.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

namespace tt::ui {

void TradePanel::scan_replay_files() {
    replay_files_.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(sessions_dir_, ec))
        if (e.is_regular_file() && e.path().extension() == ".ttk")
            replay_files_.push_back(e.path().filename().string());
    std::sort(replay_files_.rbegin(), replay_files_.rend());   // newest first
    replay_idx_ = 0;
}

void TradePanel::draw(bool* open, const std::string& strategy_name, bool polygon_available,
                      const StartFn& start, const ReplayFn& replay) {
    const bool visible = ImGui::Begin("Trade", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    const LiveSnapshot s = eng_.live_snapshot();

    if (!s.running) {
        ImGui::TextDisabled("Paper trading — %s", strategy_name.c_str());

        ImGui::SetNextItemWidth(80);
        const bool entered = ImGui::InputText("##add_sym", input_, sizeof(input_),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        if ((ImGui::Button("Add") || entered) && input_[0]) {
            std::string sym(input_);
            std::transform(sym.begin(), sym.end(), sym.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (std::find(pending_symbols_.begin(), pending_symbols_.end(), sym) ==
                pending_symbols_.end())
                pending_symbols_.push_back(sym);
            input_[0] = '\0';
        }
        int remove_at = -1;
        for (size_t i = 0; i < pending_symbols_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(pending_symbols_[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_at = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove_at >= 0) pending_symbols_.erase(pending_symbols_.begin() + remove_at);

        ImGui::SetNextItemWidth(100);
        ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("bar sec", &bar_sec_);
        bar_sec_ = std::clamp(bar_sec_, 1, 3600);

        static constexpr const char* kBrokers[] = {"Simulator", "IBKR (gateway)"};
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("broker", &broker_idx_, kBrokers, IM_ARRAYSIZE(kBrokers));
        ImGui::SetItemTooltip("Simulator: local fills (ExecSim).\n"
                              "IBKR: Client Portal Gateway on this machine — "
                              "Account menu > Sign In > IBKR to connect.");
        static constexpr const char* kData[] = {"IBKR (gateway)", "Polygon"};
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("data", &data_idx_, kData, IM_ARRAYSIZE(kData));
        ImGui::SetItemTooltip("IBKR: ~250 ms conflated top-of-book via the gateway "
                              "session — no extra data bill.\n"
                              "Polygon: full tick stream, needs a Polygon key "
                              "(Account menu or POLYGON_API_KEY).");
        if (data_idx_ == 1 && !polygon_available) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "needs a Polygon key");
        }
        ImGui::Checkbox("Record ticks for replay", &record_ticks_);
        ImGui::SetItemTooltip("Capture every tick to a .ttk file — replay the exact "
                              "session later, deterministically");

        if (ImGui::CollapsingHeader("Risk limits")) {
            ImGui::SetNextItemWidth(90);
            ImGui::InputDouble("max order qty", &risk_.max_order_qty, 0, 0, "%.0f");
            ImGui::SetNextItemWidth(90);
            ImGui::InputDouble("max position qty", &risk_.max_position_qty, 0, 0, "%.0f");
            ImGui::SetNextItemWidth(90);
            ImGui::InputDouble("daily max loss $", &risk_.daily_max_loss, 0, 0, "%.0f");
            ImGui::SetItemTooltip("Auto kill switch when equity drops this much below "
                                  "the session start. 0 = off");
            ImGui::SetNextItemWidth(90);
            ImGui::InputDouble("max drawdown %", &risk_dd_pct_, 0, 0, "%.1f");
            ImGui::SetItemTooltip("Auto kill switch this far below the session equity "
                                  "high. 0 = off");
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("stale feed sec", &risk_.stale_feed_sec);
            ImGui::SetItemTooltip("Auto kill switch when no ticks arrive for this long "
                                  "while a position is open. 0 = off");
            risk_.stale_feed_sec = std::max(0, risk_.stale_feed_sec);
            risk_dd_pct_ = std::clamp(risk_dd_pct_, 0.0, 99.0);
        }

        ImGui::BeginDisabled(eng_.running());   // not while a backtest runs
        if (ImGui::Button("Start paper trading") && !pending_symbols_.empty() && start) {
            session_broker_ = broker_idx_;
            StartOpts opts;
            opts.symbols = pending_symbols_;
            opts.cash = cash_;
            opts.bar_seconds = bar_sec_;
            opts.broker = static_cast<Broker>(session_broker_);
            int data = data_idx_;
            if (data == 1 && !polygon_available)
                data = 0;   // no Polygon key: fall back to gateway data
            opts.data = static_cast<DataFeed>(data);
            opts.record = record_ticks_;
            opts.risk = risk_;
            opts.risk.max_drawdown_pct = risk_dd_pct_ / 100.0;
            start(opts);
        }
        ImGui::EndDisabled();

        // ---- replay a captured session ----
        ImGui::Separator();
        ImGui::TextDisabled("Session replay");
        if (!replay_scanned_) {
            scan_replay_files();
            replay_scanned_ = true;
        }
        if (replay_files_.empty()) {
            ImGui::TextDisabled("(no .ttk files yet — record a session first)");
        } else {
            if (replay_idx_ >= static_cast<int>(replay_files_.size())) replay_idx_ = 0;
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##replay_file", replay_files_[replay_idx_].c_str())) {
                for (int i = 0; i < static_cast<int>(replay_files_.size()); ++i)
                    if (ImGui::Selectable(replay_files_[i].c_str(), i == replay_idx_))
                        replay_idx_ = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(eng_.running());
            if (ImGui::Button("Replay") && replay)
                replay(sessions_dir_ + "\\" + replay_files_[replay_idx_]);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Re-run these ticks through the current strategy + "
                                  "fill simulator; results land in the Backtest panel");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("refresh")) scan_replay_files();

        ImGui::End();
        return;
    }

    // ---- running session ----
    std::string sym_list;
    for (const SymbolState& sym : s.symbols)
        sym_list += (sym_list.empty() ? "" : ", ") + sym.symbol;
    ImGui::Text("%s", sym_list.c_str());
    ImGui::SameLine();
    if (s.halted)
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "HALTED");
    else if (session_broker_ == 1)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1), "LIVE (ibkr)");
    else
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "LIVE (paper)");

    char tbuf[16] = "--";
    if (s.last_tick_ts_ms > 0) {
        const std::time_t t = static_cast<std::time_t>(s.last_tick_ts_ms / 1000);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    ImGui::Text("ticks %llu%s   last tick @ %s", static_cast<unsigned long long>(s.ticks),
                s.dropped_ticks ? " (drops!)" : "", tbuf);
    if (s.lat_count > 0)
        ImGui::Text("tick->order p50 %.1f us   p99 %.1f us   max %.1f us   (%llu)",
                    s.lat_p50 / 1000.0, s.lat_p99 / 1000.0, s.lat_max / 1000.0,
                    static_cast<unsigned long long>(s.lat_count));
    ImGui::Text("equity %.2f   cash %.2f", s.equity, s.cash);
    for (const SymbolState& sym : s.symbols)
        ImGui::Text("  %s   last %.2f   pos %.0f", sym.symbol.c_str(), sym.last_price,
                    sym.position.qty);

    ImGui::Separator();
    if (selected_symbol_idx_ >= static_cast<int>(s.symbols.size())) selected_symbol_idx_ = 0;
    if (s.symbols.size() > 1) {
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##manual_sym", s.symbols[selected_symbol_idx_].symbol.c_str())) {
            for (int i = 0; i < static_cast<int>(s.symbols.size()); ++i)
                if (ImGui::Selectable(s.symbols[i].symbol.c_str(), i == selected_symbol_idx_))
                    selected_symbol_idx_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("##mqty", &manual_qty_, 0, 0, "%.0f");
    ImGui::SameLine();
    const uint32_t manual_sid = static_cast<uint32_t>(selected_symbol_idx_ + 1);
    if (ImGui::Button("Buy"))
        eng_.submit_manual(manual_sid, true, manual_qty_, manual_tp_, manual_sl_);
    ImGui::SameLine();
    if (ImGui::Button("Sell"))
        eng_.submit_manual(manual_sid, false, manual_qty_, manual_tp_, manual_sl_);
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("TP", &manual_tp_, 0, 0, "%.2f");
    ImGui::SetItemTooltip("Bracket take-profit price for manual orders (0 = off)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("SL", &manual_sl_, 0, 0, "%.2f");
    ImGui::SetItemTooltip("Bracket stop-loss trigger for manual orders (0 = off)");

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1));
    if (ImGui::Button("KILL SWITCH", ImVec2(-1, 0))) eng_.kill_switch();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Cancel all orders, flatten the position, halt the strategy");

    if (ImGui::Button("Stop session", ImVec2(-1, 0))) eng_.stop_live();

    ImGui::End();
}

} // namespace tt::ui
