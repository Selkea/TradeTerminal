#include "panels/trade.h"

#include "imgui.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

namespace tt::ui {

void TradePanel::draw(bool* open, const std::string& strategy_name, bool polygon_available,
                      bool finnhub_available, bool ibkr_ready, const AccountInfo& account,
                      const StartFn& start) {
    (void)strategy_name;
    const bool visible = ImGui::Begin("Trade", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    const LiveSnapshot s = eng_.live_snapshot();

    if (!s.running) {
        // ---- top row: active account (left) + data feed (right) ----
        if (ibkr_ready && !account.label.empty()) {
            ImGui::TextUnformatted(account.label.c_str());
            ImGui::SameLine();
            if (account.kind == 2)
                ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1), "LIVE");
            else if (account.kind == 1)
                ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "PAPER");
            if (account.readonly) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "read-only");
            }
        } else {
            ImGui::TextDisabled("Simulator - sign in to route to IBKR");
        }
        static constexpr const char* kData[] = {"IBKR (gateway)", "Polygon", "Finnhub"};
        const float combo_w = 120.0f;
        const float lbl_w =
            ImGui::CalcTextSize("data").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - combo_w - lbl_w -
                                          ImGui::GetStyle().WindowPadding.x - 6.0f));
        ImGui::SetNextItemWidth(combo_w);
        ImGui::Combo("data", &data_idx_, kData, IM_ARRAYSIZE(kData));
        ImGui::SetItemTooltip("IBKR: ~250 ms conflated top-of-book via the gateway "
                              "session — no extra data bill.\n"
                              "Polygon: full tick stream, needs a Polygon key.\n"
                              "Finnhub: real-time US trade prints, free key.");
        if (data_idx_ == 1 && !polygon_available)
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "data feed needs a Polygon key");
        else if (data_idx_ == 2 && !finnhub_available)
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "data feed needs a Finnhub key");

        // Shared cash pool for the simulator. A real IBKR account uses its own
        // balance; with sub-accounts, each symbol picks one instead (below).
        if (!ibkr_ready) {
            ImGui::SetNextItemWidth(100);
            ImGui::InputDouble("cash", &session_cash_, 0, 0, "%.0f");
            ImGui::SetItemTooltip("Simulator starting cash, shared across symbols");
        }

        // ---- add a symbol ----
        ImGui::SetNextItemWidth(80);
        const bool entered = ImGui::InputText("##add_sym", input_, sizeof(input_),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        if ((ImGui::Button("Add") || entered) && input_[0]) {
            std::string sym(input_);
            std::transform(sym.begin(), sym.end(), sym.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            const bool dup = std::any_of(pending_.begin(), pending_.end(),
                                         [&](const SymRow& r) { return r.symbol == sym; });
            if (!dup) pending_.push_back({sym, def_bar_sec_, def_record_, 0});
            input_[0] = '\0';
        }

        // ---- per-symbol cards: each symbol its own cash / bar size / record ----
        int remove_at = -1;
        for (size_t i = 0; i < pending_.size(); ++i) {
            SymRow& r = pending_[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string hdr = r.symbol + "###sym";   // stable id across renames
            if (ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();
                // Capital: pick a sub-account when the login has them, else the
                // shared account/pool.
                if (account.subaccounts.size() > 1) {
                    r.account_idx = std::clamp(
                        r.account_idx, 0, static_cast<int>(account.subaccounts.size()) - 1);
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::BeginCombo("cash",
                                          account.subaccounts[r.account_idx].c_str())) {
                        for (int a = 0; a < static_cast<int>(account.subaccounts.size()); ++a)
                            if (ImGui::Selectable(account.subaccounts[a].c_str(),
                                                  a == r.account_idx))
                                r.account_idx = a;
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip("Sub-account this symbol trades in");
                } else {
                    ImGui::TextDisabled("cash: shared");
                }
                ImGui::SameLine();
                ImGui::Checkbox("Record", &r.record);
                ImGui::SetItemTooltip("Capture this symbol's ticks to a .ttk file for replay");
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("bars/sec", &r.bar_sec, 1, 10);
                r.bar_sec = std::clamp(r.bar_sec, 1, 3600);
                if (ImGui::SmallButton("remove")) remove_at = static_cast<int>(i);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (remove_at >= 0) pending_.erase(pending_.begin() + remove_at);
        // A newly added symbol inherits the last card's settings.
        if (!pending_.empty()) {
            def_bar_sec_ = pending_.back().bar_sec;
            def_record_ = pending_.back().record;
        }

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
        if (ImGui::Button("Start Trading") && !pending_.empty() && start) {
            session_broker_ = ibkr_ready ? 1 : 0;   // IBKR if signed in, else Simulator
            StartOpts opts;
            opts.broker = static_cast<Broker>(session_broker_);
            int data = data_idx_;
            if (data == 1 && !polygon_available)
                data = 0;   // no Polygon key: fall back to gateway data
            if (data == 2 && !finnhub_available)
                data = 0;   // no Finnhub key: fall back to gateway data
            opts.data = static_cast<DataFeed>(data);
            opts.session_cash = session_cash_;
            for (const SymRow& r : pending_) {
                const std::string acct =
                    account.subaccounts.size() > 1 &&
                            r.account_idx < static_cast<int>(account.subaccounts.size())
                        ? account.subaccounts[r.account_idx] : std::string();
                opts.symbols.push_back({r.symbol, r.bar_sec, r.record, acct});
            }
            opts.risk = risk_;
            opts.risk.max_drawdown_pct = risk_dd_pct_ / 100.0;
            start(opts);
        }
        ImGui::EndDisabled();

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
