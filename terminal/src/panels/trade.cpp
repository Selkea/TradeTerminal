#include "panels/trade.h"

#include "imgui.h"
#include "imgui_internal.h"   // GetCurrentTabBar: overflow-aware tab-list button
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace tt::ui {

namespace {
// "HH:MM" (24h) -> minutes since midnight, or -1 if malformed.
int parse_hhmm(const char* s) {
    int h = -1, m = -1;
    if (std::sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}
} // namespace

void TradePanel::restore_schedule(bool on, const std::string& start,
                                  const std::string& stop) {
    sched_on_ = on;
    if (!start.empty() && start.size() < sizeof sched_start_)
        std::snprintf(sched_start_, sizeof sched_start_, "%s", start.c_str());
    if (!stop.empty() && stop.size() < sizeof sched_stop_)
        std::snprintf(sched_stop_, sizeof sched_stop_, "%s", stop.c_str());
}

void TradePanel::set_symbol_strategy(const std::string& symbol, const std::string& key,
                                     const std::map<std::string, double>& params) {
    for (SymRow& r : pending_)
        if (r.symbol == symbol) {
            r.strat_key = key;
            r.params = params;
            return;
        }
    pending_.push_back({symbol, def_bar_sec_, def_record_, 0, def_risk_,
                        def_risk_dd_pct_, key, params, def_ap_mode_, def_ap_trigger_,
                        def_ap_interval_min_, def_ap_dd_pct_});
}

bool TradePanel::store_symbol_params(const std::string& symbol, const std::string& key,
                                     const std::map<std::string, double>& params) {
    for (SymRow& r : pending_)
        if (r.symbol == symbol && r.strat_key == key) {
            r.params = params;
            return true;
        }
    return false;
}

std::vector<std::string> TradePanel::pending_symbols() const {
    std::vector<std::string> out;
    out.reserve(pending_.size());
    for (const SymRow& r : pending_) out.push_back(r.symbol);
    return out;
}

bool TradePanel::has_own_params(const std::string& symbol) const {
    for (const SymRow& r : pending_)
        if (r.symbol == symbol) return !r.params.empty();
    return false;
}

int TradePanel::remove_symbols(const std::vector<std::string>& symbols) {
    const size_t before = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const SymRow& r) {
                                      return std::find(symbols.begin(), symbols.end(),
                                                       r.symbol) != symbols.end();
                                  }),
                   pending_.end());
    if (pending_.empty()) selected_symbol_idx_ = 0;
    else selected_symbol_idx_ =
             std::min(selected_symbol_idx_, static_cast<int>(pending_.size()) - 1);
    want_tab_ = pending_.empty() ? -1 : selected_symbol_idx_;
    return static_cast<int>(before - pending_.size());
}

void TradePanel::set_lineup(const std::vector<std::string>& symbols) {
    if (symbols.empty()) return;   // don't strip the panel down to no tabs
    // Carry over a repeat pick's OWN (strategy, params) pair before the tabs are
    // rebuilt. Until 0.16.0 this cleared params unconditionally, so a symbol
    // whose tournament then failed had nothing of its own left and inherited
    // whatever the shared per-strategy map happened to hold — on 2026-08-10, a
    // set fitted to SSPC. The pair moves together on purpose: params fitted for
    // one strategy mean nothing to another that happens to declare the same
    // parameter names.
    std::vector<SymRow> prev;
    prev.swap(pending_);
    for (const std::string& sym : symbols) {
        SymRow row{sym,     def_bar_sec_,     def_record_,    0,
                   def_risk_, def_risk_dd_pct_, def_strat_key_, {},
                   def_ap_mode_, def_ap_trigger_, def_ap_interval_min_, def_ap_dd_pct_};
        for (const SymRow& old : prev)
            if (old.symbol == sym && !old.params.empty()) {
                row.strat_key = old.strat_key;
                row.params = old.params;
                break;
            }
        pending_.push_back(std::move(row));
    }
    selected_symbol_idx_ = 0;
    want_tab_ = 0;
}

TradePanel::StartOpts TradePanel::build_start_opts(const AccountInfo& account,
                                                   const ParamSpecsFn& strat_params,
                                                   bool polygon_available,
                                                   bool finnhub_available, bool ibkr_ready) {
    // TWS route is explicit; web route follows the sign-in (else sim).
    session_broker_ = route_ == 1 ? 2 : (ibkr_ready ? 1 : 0);
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
        RiskLimits rk = r.risk;
        rk.max_drawdown_pct = r.risk_dd_pct / 100.0;   // percent -> fraction
        // This symbol's params for its current strategy: its OWN value for every
        // declared name it has one for, the strategy's current value for the
        // rest. That fallback is what made 2026-08-10 invisible — six tabs with
        // params={} all silently took the same shared set — so the selection now
        // reports where each name came from and the caller logs it.
        std::vector<ParamDefault> declared;
        for (const StratParam& sp : strat_params(r.strat_key))
            declared.push_back({sp.name, sp.value});
        ParamSelection sel = select_symbol_params(r.params, declared);

        SymbolOpt so;
        so.symbol = r.symbol;
        so.bar_seconds = r.bar_sec;
        so.record = r.record;
        so.account = acct;
        so.strat_key = r.strat_key;
        so.params = std::move(sel.params);
        so.param_source = sel.source;
        so.inherited_params = std::move(sel.inherited_names);
        so.risk = rk;
        so.ap_mode = r.ap_mode;
        so.ap_trigger = r.ap_trigger;
        so.ap_interval_min = r.ap_interval_min;
        so.ap_dd_pct = r.ap_dd_pct;
        opts.symbols.push_back(std::move(so));
    }
    return opts;
}

void TradePanel::draw(bool* open, const std::vector<std::string>& strat_sources,
                      const ParamSpecsFn& strat_params, const StratNameFn& strat_name,
                      const AutoPickFn& autopick, bool polygon_available,
                      bool finnhub_available, bool ibkr_ready, const AccountInfo& account,
                      const StartFn& start, const SymbolPickFn& chart_pick) {
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
        static constexpr const char* kData[] = {"IBKR (web)", "Polygon", "Finnhub",
                                                "IBKR (TWS)"};
        const float combo_w = 120.0f;
        const float lbl_w =
            ImGui::CalcTextSize("data").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - combo_w - lbl_w -
                                          ImGui::GetStyle().WindowPadding.x - 6.0f));
        ImGui::SetNextItemWidth(combo_w);
        ImGui::Combo("data", &data_idx_, kData, IM_ARRAYSIZE(kData));
        ImGui::SetItemTooltip("IBKR (web): ~250 ms conflated top-of-book via the CP "
                              "gateway session — no extra data bill.\n"
                              "Polygon: full tick stream, needs a Polygon key.\n"
                              "Finnhub: real-time US trade prints, free key.\n"
                              "IBKR (TWS): tick-by-tick via IB Gateway's socket API "
                              "(real-time needs a market-data subscription).");
        if (data_idx_ == 1 && !polygon_available)
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "data feed needs a Polygon key");
        else if (data_idx_ == 2 && !finnhub_available)
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "data feed needs a Finnhub key");

        // Order route: which IBKR door orders go through.
        ImGui::AlignTextToFramePadding();
        if (route_ == 1)
            ImGui::TextDisabled("broker: IBKR via TWS socket");
        else if (ibkr_ready)
            ImGui::TextDisabled("broker: IBKR via web API");
        else
            ImGui::TextDisabled("broker: Simulator (sign in to route to IBKR)");
        ImGui::SameLine();
        static constexpr const char* kRoutes[] = {"Web API", "TWS"};
        ImGui::SetNextItemWidth(90);
        ImGui::Combo("route", &route_, kRoutes, IM_ARRAYSIZE(kRoutes));
        ImGui::SetItemTooltip(
            "Which IBKR interface orders use.\n"
            "Web API: the Client Portal gateway (~75 ms orders; auto-login).\n"
            "TWS: IB Gateway's socket API (~5-20 ms orders; needs IB Gateway "
            "running and logged in, port 4002 paper / 4001 live).");

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
            if (!dup)
                pending_.push_back({sym, def_bar_sec_, def_record_, 0, def_risk_,
                                    def_risk_dd_pct_, def_strat_key_, {}, def_ap_mode_,
                                    def_ap_trigger_, def_ap_interval_min_, def_ap_dd_pct_});
            input_[0] = '\0';
        }

        // ---- per-symbol tabs: each symbol its own cash / bar size / record ----
        int remove_at = -1;
        // Each row's ImGui tab id, captured in the tab-bar id scope so we can
        // read the user's drag-reordered order back out below.
        std::vector<ImGuiID> row_tab_id(pending_.size());
        if (!pending_.empty() &&
            ImGui::BeginTabBar("##symtabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                                ImGuiTabBarFlags_Reorderable |
                                                ImGuiTabBarFlags_FittingPolicyScroll |
                                                ImGuiTabBarFlags_NoTabListScrollingButtons)) {
            // Tabs keep full width (scroll instead of shrink). Show the tab-list
            // button only when they overflow — i.e. a resize-down would have
            // occurred — by peeking ImGui's ideal-vs-available width (last frame).
            if (const ImGuiTabBar* tb = ImGui::GetCurrentTabBar();
                tb && tb->WidthAllTabsIdeal > tb->BarRect.GetWidth() + 1.0f) {
                if (ImGui::TabItemButton("  ##symtablist", ImGuiTabItemFlags_Leading |
                                                               ImGuiTabItemFlags_NoTooltip))
                    ImGui::OpenPopup("##symtablist");
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
                const float rr = ImGui::GetFontSize() * 0.26f;
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddTriangleFilled(ImVec2(cx - rr, cy - rr * 0.5f),
                                      ImVec2(cx + rr, cy - rr * 0.5f),
                                      ImVec2(cx, cy + rr * 0.7f), col);
            }
            for (size_t i = 0; i < pending_.size(); ++i) {
                SymRow& r = pending_[i];
                bool open = true;
                // Tab id = symbol (unique, stable across add/remove); the tab's
                // close button removes it. want_tab_ selects it from the button.
                // Same hash ImGui gives the tab (label id in the tab-bar scope).
                row_tab_id[i] = ImGui::GetID(r.symbol.c_str());
                const ImGuiTabItemFlags sel =
                    want_tab_ == static_cast<int>(i) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(r.symbol.c_str(), &open, sel)) {
                    ImGui::PushID(r.symbol.c_str());
                    // Capital: sub-account picker when the login has them, else the
                    // shared account/pool.
                    if (account.subaccounts.size() > 1) {
                        r.account_idx = std::clamp(
                            r.account_idx, 0,
                            static_cast<int>(account.subaccounts.size()) - 1);
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::BeginCombo("cash",
                                              account.subaccounts[r.account_idx].c_str())) {
                            for (int a = 0;
                                 a < static_cast<int>(account.subaccounts.size()); ++a)
                                if (ImGui::Selectable(account.subaccounts[a].c_str(),
                                                      a == r.account_idx))
                                    r.account_idx = a;
                            ImGui::EndCombo();
                        }
                        ImGui::SetItemTooltip("Sub-account this symbol trades in");
                    } else {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("cash: shared");
                    }
                    // Record: pinned to the top-right of the tab, on the cash row.
                    const float rec_w = ImGui::GetFrameHeight() +
                                        ImGui::GetStyle().ItemInnerSpacing.x +
                                        ImGui::CalcTextSize("Record").x;
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - rec_w);
                    ImGui::Checkbox("Record", &r.record);
                    ImGui::SetItemTooltip("Capture this symbol's ticks to a .ttk file "
                                          "for replay");
                    // Strategy for this symbol (built-in SMA or a loaded source),
                    // shown by display name but stored by key. strat_sources is
                    // alphabetical by display name and already includes "".
                    ImGui::SetNextItemWidth(220);
                    if (ImGui::BeginCombo("strategy", strat_name(r.strat_key).c_str())) {
                        for (const std::string& src : strat_sources) {
                            const std::string lbl = strat_name(src) + "###" + src;
                            if (ImGui::Selectable(lbl.c_str(), src == r.strat_key))
                                r.strat_key = src;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip("Strategy this symbol trades");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Auto-pick") && autopick) autopick(r.symbol);
                    ImGui::SetItemTooltip("Tournament: optimize every loaded strategy on "
                                          "recent data (Optimizer panel's settings) and "
                                          "apply the best holdout score here");
                    // This symbol's own copy of the strategy's parameters.
                    const std::vector<StratParam> specs = strat_params(r.strat_key);
                    if (!specs.empty() && ImGui::CollapsingHeader("Parameters")) {
                        for (const StratParam& sp : specs) {
                            double& v =
                                r.params.try_emplace(sp.name, sp.value).first->second;
                            ImGui::SetNextItemWidth(120);
                            ImGui::InputDouble(sp.name.c_str(), &v, 0, 0, "%.4g");
                            if (sp.min < sp.max) v = std::clamp(v, sp.min, sp.max);
                        }
                    }
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputInt("bar sec", &r.bar_sec, 1, 10);
                    r.bar_sec = std::clamp(r.bar_sec, 1, 3600);
                    // Autopilot: re-optimize this symbol while it trades.
                    static constexpr const char* kApModes[] = {"Off", "Params", "Full"};
                    static constexpr const char* kApTrigs[] = {"Timer", "Drawdown",
                                                               "Both"};
                    ImGui::SetNextItemWidth(80);
                    ImGui::Combo("autopilot", &r.ap_mode, kApModes,
                                 IM_ARRAYSIZE(kApModes));
                    ImGui::SetItemTooltip(
                        "Re-optimize this symbol while it trades (applied only while "
                        "flat, holdout-scored with hysteresis).\n"
                        "Params: re-tune the current strategy's parameters.\n"
                        "Full: params + the strategy itself can be swapped when a "
                        "challenger wins decisively twice in a row.");
                    if (r.ap_mode > 0) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(90);
                        ImGui::Combo("##aptrig", &r.ap_trigger, kApTrigs,
                                     IM_ARRAYSIZE(kApTrigs));
                        ImGui::SetItemTooltip("What triggers a re-optimization cycle");
                        if (r.ap_trigger != 1) {   // timer involved
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(44);
                            ImGui::InputDouble("min##apmin", &r.ap_interval_min, 0, 0,
                                               "%.0f");
                            r.ap_interval_min = std::clamp(r.ap_interval_min, 5.0, 480.0);
                        }
                        if (r.ap_trigger >= 1) {   // drawdown involved
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(44);
                            ImGui::InputDouble("dd%##apdd", &r.ap_dd_pct, 0, 0, "%.1f");
                            ImGui::SetItemTooltip("Re-optimize when session equity is "
                                                  "this far below its high (10 min "
                                                  "cooldown)");
                            r.ap_dd_pct = std::clamp(r.ap_dd_pct, 0.5, 50.0);
                        }
                    }
                    if (ImGui::CollapsingHeader("Risk limits")) {
                        ImGui::SetNextItemWidth(90);
                        ImGui::InputDouble("max order qty", &r.risk.max_order_qty, 0, 0,
                                           "%.0f");
                        ImGui::SetNextItemWidth(90);
                        ImGui::InputDouble("max position qty", &r.risk.max_position_qty, 0,
                                           0, "%.0f");
                        ImGui::SetNextItemWidth(90);
                        ImGui::InputDouble("daily max loss $", &r.risk.daily_max_loss, 0, 0,
                                           "%.0f");
                        ImGui::SetItemTooltip("Kill switch when this symbol's equity drops "
                                              "this much below the session start. 0 = off");
                        ImGui::SetNextItemWidth(90);
                        ImGui::InputDouble("max drawdown %", &r.risk_dd_pct, 0, 0, "%.1f");
                        ImGui::SetItemTooltip("Kill switch this far below the session equity "
                                              "high. 0 = off");
                        ImGui::SetNextItemWidth(90);
                        ImGui::InputInt("stale feed sec", &r.risk.stale_feed_sec);
                        ImGui::SetItemTooltip("Kill switch when no ticks arrive for this "
                                              "long while a position is open. 0 = off");
                        r.risk.stale_feed_sec = std::max(0, r.risk.stale_feed_sec);
                        r.risk_dd_pct = std::clamp(r.risk_dd_pct, 0.0, 99.0);
                        ImGui::Checkbox("hold — don't halt", &r.risk.disable_auto_halt);
                        ImGui::SetItemTooltip("Don't auto-flatten on the daily-loss / drawdown "
                                              "limits — hold positions until the strategy "
                                              "exits or they recover. The size cap and "
                                              "stale-feed guard still apply. Removes the "
                                              "session's daily-loss safety net.");
                    }
                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
                if (!open) remove_at = static_cast<int>(i);
            }
            // Persist drag-reordered tabs: ImGui reorders its own Tabs array in
            // place, but our pending_ (what symbols_config() saves) doesn't
            // follow unless we sync it -- so the new order was lost on restart.
            // Rebuild pending_ in the tab bar's current visual order. Skipped on
            // a removal frame (tab count is about to change below).
            if (remove_at < 0) {
                if (const ImGuiTabBar* tbm = ImGui::GetCurrentTabBar()) {
                    std::vector<size_t> perm;
                    perm.reserve(pending_.size());
                    for (const ImGuiTabItem& t : tbm->Tabs)
                        for (size_t i = 0; i < pending_.size(); ++i)
                            if (row_tab_id[i] == t.ID) {   // non-symbol tabs (the
                                perm.push_back(i);          // list button) match none
                                break;
                            }
                    bool changed = perm.size() == pending_.size();
                    if (changed) {
                        changed = false;
                        for (size_t i = 0; i < perm.size(); ++i)
                            if (perm[i] != i) { changed = true; break; }
                    }
                    if (changed) {
                        std::vector<SymRow> reordered;
                        reordered.reserve(pending_.size());
                        for (size_t idx : perm)
                            reordered.push_back(std::move(pending_[idx]));
                        pending_ = std::move(reordered);
                    }
                }
            }
            want_tab_ = -1;   // consumed by the SetSelected above
            if (ImGui::BeginPopup("##symtablist")) {
                for (size_t i = 0; i < pending_.size(); ++i)
                    if (ImGui::Selectable(pending_[i].symbol.c_str()))
                        want_tab_ = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::EndTabBar();
        }
        if (remove_at >= 0) pending_.erase(pending_.begin() + remove_at);
        // A newly added symbol inherits the last tab's settings.
        if (!pending_.empty()) {
            def_bar_sec_ = pending_.back().bar_sec;
            def_record_ = pending_.back().record;
            def_risk_ = pending_.back().risk;
            def_risk_dd_pct_ = pending_.back().risk_dd_pct;
            def_strat_key_ = pending_.back().strat_key;
            def_ap_mode_ = pending_.back().ap_mode;
            def_ap_trigger_ = pending_.back().ap_trigger;
            def_ap_interval_min_ = pending_.back().ap_interval_min;
            def_ap_dd_pct_ = pending_.back().ap_dd_pct;
        }

        auto do_start = [&] {
            start(build_start_opts(account, strat_params, polygon_available,
                                   finnhub_available, ibkr_ready));
        };

        ImGui::BeginDisabled(eng_.running());   // not while a backtest runs
        if (ImGui::Button("Start Trading") && !pending_.empty() && start) do_start();
        ImGui::EndDisabled();

        // ---- session schedule (auto start/stop, local time, weekdays) ----
        ImGui::SameLine();
        ImGui::Checkbox("auto", &sched_on_);
        ImGui::SetItemTooltip(
            "Start the session at the first time and stop it at the second\n"
            "(local clock, weekdays only). The stop cancels orders and flattens\n"
            "positions via the kill switch. A manually stopped session does not\n"
            "auto-restart the same day.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(48);
        ImGui::InputText("##schedstart", sched_start_, sizeof sched_start_);
        ImGui::SameLine();
        ImGui::TextUnformatted("-");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(48);
        ImGui::InputText("##schedstop", sched_stop_, sizeof sched_stop_);

        // Auto-start: level-triggered inside the window so a reboot mid-morning
        // still brings the session up; the once-per-day guard (also set while a
        // session runs, below) keeps a manual stop from bouncing right back.
        if (sched_on_ && !pending_.empty() && start && !eng_.running()) {
            const int start_min = parse_hhmm(sched_start_);
            const int stop_min = parse_hhmm(sched_stop_);
            std::time_t now_tt = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now_tt);
            const int now_min = tm.tm_hour * 60 + tm.tm_min;
            const bool weekday = tm.tm_wday >= 1 && tm.tm_wday <= 5;
            if (start_min >= 0 && stop_min >= 0 && weekday &&
                tm.tm_yday != sched_last_start_day_ && now_min >= start_min &&
                now_min < stop_min) {
                sched_last_start_day_ = tm.tm_yday;
                do_start();
            }
        }

        ImGui::End();
        return;
    }

    // ---- running session ----
    // Scheduled stop: cancel + flatten + stop when the local clock crosses the
    // stop time. Edge-triggered, so a session started manually after hours is
    // left alone. While anything runs, mark today as "started" so a manual
    // stop is not followed by a same-day auto-restart.
    if (sched_on_) {
        std::time_t now_tt = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now_tt);
        const int now_min = tm.tm_hour * 60 + tm.tm_min;
        const int stop_min = parse_hhmm(sched_stop_);
        sched_last_start_day_ = tm.tm_yday;
        if (stop_min >= 0 && sched_prev_min_ >= 0 && sched_prev_min_ < stop_min &&
            now_min >= stop_min) {
            eng_.kill_switch();   // cancel all orders + flatten positions
            eng_.stop_live();     // graceful stop, joins the live thread
        }
        sched_prev_min_ = now_min;
    }

    // ---- line 1: account + PAPER/LIVE tag (left), equity/cash (mid), feed (right) ----
    ImGui::TextUnformatted(account.label.empty() ? "Simulator" : account.label.c_str());
    ImGui::SameLine();
    if (account.kind == 2)
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1), "LIVE");
    else if (account.kind == 1)
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "PAPER");
    else
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "SIM");
    if (account.readonly) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "read-only");
    }
    if (s.halted) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "HALTED");
    }
    ImGui::SameLine();
    ImGui::Text("   equity %.2f   cash %.2f", s.equity, s.cash);
    if (sched_on_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(auto-stop %s)", sched_stop_);
    }
    {
        static const char* const kFeed[] = {"IBKR (web)", "Polygon", "Finnhub", "IBKR (TWS)"};
        const char* feed = (data_idx_ >= 0 && data_idx_ < 4) ? kFeed[data_idx_] : "?";
        const float fw = ImGui::CalcTextSize(feed).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - fw -
                                          ImGui::GetStyle().WindowPadding.x - 6.0f));
        ImGui::TextDisabled("%s", feed);
    }

    // ---- line 2: sortable positions table (mirrors the Positions panel).
    // Sortable columns persist to imgui.ini; clicking a row loads it on the Chart.
    {
        struct Row { const SymbolState* s; double bid; double ask; };
        std::vector<Row> rows;
        rows.reserve(s.symbols.size());
        for (const SymbolState& sym : s.symbols) {
            Quote rq;
            const bool has = quotes_.get(sym.symbol, rq);
            rows.push_back({&sym, has ? rq.bid : 0.0, has ? rq.ask : 0.0});
        }
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_Sortable;
        if (ImGui::BeginTable("##running_pos", 8, tflags)) {
            ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("Avg");
            ImGui::TableSetupColumn("Bid");
            ImGui::TableSetupColumn("Ask");
            ImGui::TableSetupColumn("Last");
            ImGui::TableSetupColumn("Unrealized");
            ImGui::TableSetupColumn("Realized");
            ImGui::TableHeadersRow();

            // Live-sort every frame so values re-order as prices/PnL move.
            if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& sp = ss->Specs[0];
                const bool asc = sp.SortDirection != ImGuiSortDirection_Descending;
                auto num = [&](const Row& r) -> double {
                    switch (sp.ColumnIndex) {
                    case 1: return r.s->position.qty;
                    case 2: return r.s->position.avg_price;
                    case 3: return r.bid;
                    case 4: return r.ask;
                    case 5: return r.s->last_price;
                    case 6: return r.s->position.unrealized_pnl;
                    case 7: return r.s->position.realized_pnl;
                    default: return 0.0;
                    }
                };
                std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                    if (sp.ColumnIndex == 0)
                        return asc ? a.s->symbol < b.s->symbol : a.s->symbol > b.s->symbol;
                    const double x = num(a), y = num(b);
                    return asc ? x < y : x > y;
                });
                ss->SpecsDirty = false;
            }

            const ImVec4 up(0.25f, 0.85f, 0.45f, 1), dn(0.9f, 0.35f, 0.3f, 1);
            for (const Row& r : rows) {
                const SymbolState& sym = *r.s;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // Whole row is clickable: load the symbol on the Chart.
                if (ImGui::Selectable(sym.symbol.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns) &&
                    chart_pick)
                    chart_pick(sym.symbol);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", sym.position.qty);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", sym.position.avg_price);
                ImGui::TableNextColumn();
                if (r.bid > 0) ImGui::Text("%.2f", r.bid); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (r.ask > 0) ImGui::Text("%.2f", r.ask); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", sym.last_price);
                ImGui::TableNextColumn();
                ImGui::TextColored(sym.position.unrealized_pnl >= 0 ? up : dn, "%+.2f",
                                   sym.position.unrealized_pnl);
                ImGui::TableNextColumn();
                ImGui::TextColored(sym.position.realized_pnl >= 0 ? up : dn, "%+.2f",
                                   sym.position.realized_pnl);
            }
            ImGui::EndTable();
        }
    }

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
    const std::string& msym = s.symbols[selected_symbol_idx_].symbol;

    // Session state (from the local = exchange clock) — display only; every
    // manual order routes as an outside-RTH limit regardless (see submit below).
    // RTH = weekday 09:30–16:00; extended = 04:00–20:00.
    std::time_t now_tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now_tt);
    const int now_min = tm.tm_hour * 60 + tm.tm_min;
    const bool weekday = tm.tm_wday >= 1 && tm.tm_wday <= 5;
    const bool in_rth = weekday && now_min >= 9 * 60 + 30 && now_min < 16 * 60;
    const bool ext_session = weekday && !in_rth && now_min >= 4 * 60 && now_min < 20 * 60;

    Quote q{};
    quotes_.get(msym, q);   // feeds the marketable-limit calc; bid/ask/last now
                            // shown per-symbol in the Positions panel
    // Marketable limit for the `buy` side: buy at the ask, sell at the bid,
    // nudged ~10 bps through to clear the spread (broker snaps to venue tick).
    // Falls back to last; 0 when no price is known.
    auto marketable = [&](bool buy) -> double {
        const double ref = buy ? (q.ask > 0 ? q.ask : q.price)
                               : (q.bid > 0 ? q.bid : q.price);
        return ref > 0 ? (buy ? ref * 1.001 : ref * 0.999) : 0.0;
    };
    // Every manual order is an outside-RTH limit: a marketable limit fills like a
    // market order in regular hours (with a worst-case price guard) and is the
    // only thing that fills in extended hours. Use the typed Lmt if given, else a
    // marketable one. With no quote to price a limit, fall back to a plain order
    // (a market that fills in RTH / queues after hours).
    auto submit = [&](bool buy) {
        const double lmt = manual_lmt_ > 0.0 ? manual_lmt_ : marketable(buy);
        eng_.submit_manual(manual_sid, buy, manual_qty_, manual_tp_, manual_sl_,
                           lmt, /*outside_rth=*/lmt > 0.0);
    };
    if (ImGui::Button("Buy")) submit(true);
    ImGui::SameLine();
    if (ImGui::Button("Sell")) submit(false);
    // Only flag the non-regular sessions; regular hours is the default (no label).
    if (!in_rth) {
        ImGui::SameLine();
        if (ext_session)
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1), "Extended hours");
        else
            ImGui::TextColored(ImVec4(0.95f, 0.50f, 0.35f, 1), "Market closed");
    }
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("Lmt", &manual_lmt_, 0, 0, "%.2f");
    ImGui::SetItemTooltip("Limit price. 0 = a marketable limit auto-computed from the quote "
                          "(buy the ask, sell the bid). Every manual order routes as an "
                          "outside-RTH limit, so it fills in regular AND extended hours.");
    ImGui::SameLine();
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

std::vector<TradeSymbol> TradePanel::symbols_config() const {
    std::vector<TradeSymbol> out;
    out.reserve(pending_.size());
    for (const SymRow& r : pending_) {
        TradeSymbol ts;
        ts.symbol = r.symbol;
        ts.bar_sec = r.bar_sec;
        ts.record = r.record;
        ts.strat_key = r.strat_key;
        ts.account_idx = r.account_idx;
        ts.risk_max_order_qty = r.risk.max_order_qty;
        ts.risk_max_position_qty = r.risk.max_position_qty;
        ts.risk_daily_max_loss = r.risk.daily_max_loss;
        ts.risk_stale_feed_sec = r.risk.stale_feed_sec;
        ts.risk_dd_pct = r.risk_dd_pct;
        ts.risk_disable_halt = r.risk.disable_auto_halt;
        ts.params = r.params;
        ts.ap_mode = r.ap_mode;
        ts.ap_trigger = r.ap_trigger;
        ts.ap_interval_min = r.ap_interval_min;
        ts.ap_dd_pct = r.ap_dd_pct;
        out.push_back(std::move(ts));
    }
    return out;
}

void TradePanel::restore_symbols(const std::vector<TradeSymbol>& syms) {
    if (syms.empty()) return;   // keep the default AAPL tab
    pending_.clear();
    for (const TradeSymbol& ts : syms) {
        SymRow r;
        r.symbol = ts.symbol;
        r.bar_sec = ts.bar_sec;
        r.record = ts.record;
        r.strat_key = ts.strat_key;
        r.account_idx = ts.account_idx;
        r.risk.max_order_qty = ts.risk_max_order_qty;
        r.risk.max_position_qty = ts.risk_max_position_qty;
        r.risk.daily_max_loss = ts.risk_daily_max_loss;
        r.risk.stale_feed_sec = ts.risk_stale_feed_sec;
        r.risk_dd_pct = ts.risk_dd_pct;
        r.risk.disable_auto_halt = ts.risk_disable_halt;
        r.params = ts.params;
        r.ap_mode = ts.ap_mode;
        r.ap_trigger = ts.ap_trigger;
        r.ap_interval_min = ts.ap_interval_min;
        r.ap_dd_pct = ts.ap_dd_pct;
        pending_.push_back(std::move(r));
    }
}

} // namespace tt::ui
