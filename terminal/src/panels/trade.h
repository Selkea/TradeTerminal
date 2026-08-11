#pragma once

#include "config.h"
#include "engine/engine.h"
#include "market_data.h"   // QuoteBook (live bid/ask for the marketable-limit calc)
#include "symbol_params.h" // own-vs-inherited parameter selection

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tt::ui {

// Live paper-trading controls: start/stop a session, manual orders, kill
// switch, and session status. (Session replay lives in the Backtest panel.)
class TradePanel {
public:
    enum class Broker : int { Sim = 0, Ibkr = 1, Tws = 2 };
    enum class DataFeed : int { Ibkr = 0, Polygon = 1, Finnhub = 2, Tws = 3 };

    // Per-symbol settings for a session (bar size, tick capture, sub-account,
    // risk limits). max_drawdown_pct is already a fraction here.
    struct SymbolOpt {
        std::string symbol;
        int bar_seconds = 60;
        bool record = true;
        std::string account;      // IBKR sub-account id; "" = shared account/pool
        std::string strat_key;    // strategy source basename; "" = built-in SMA
        std::map<std::string, double> params;   // this symbol's strategy params
        // Where `params` came from. Carried to the start path so the session log
        // and /diag can name it: a symbol silently running the shared strategy
        // default is exactly the 2026-08-10 failure, and it left no trace.
        ParamSource param_source = ParamSource::None;
        std::vector<std::string> inherited_params;   // declared names that fell back
        RiskLimits risk{};
        // Autopilot: re-optimize while trading. mode 0 off, 1 params-only,
        // 2 full (strategy can be swapped). trigger 0 timer, 1 drawdown, 2 both.
        int ap_mode = 0;
        int ap_trigger = 0;
        double ap_interval_min = 30;
        double ap_dd_pct = 5;
    };
    // A strategy's declared parameter, its current value, and range.
    struct StratParam {
        std::string name;
        double value, min, max;
    };
    // Resolve a strategy key's parameters (for per-symbol editing).
    using ParamSpecsFn = std::function<std::vector<StratParam>(const std::string& key)>;
    // Resolve a strategy key ("" = built-in) to its display name.
    using StratNameFn = std::function<std::string(const std::string& key)>;
    // Run a strategy tournament for this symbol (auto-pick the champion).
    using AutoPickFn = std::function<void(const std::string& symbol)>;
    // Load a symbol into the Chart panel (running-session table row click).
    using SymbolPickFn = std::function<void(const std::string& symbol)>;
    struct StartOpts {
        std::vector<SymbolOpt> symbols;   // one entry per traded symbol
        double session_cash = 100'000.0;  // shared pool (simulator / single account)
        Broker broker = Broker::Sim;      // where orders route
        DataFeed data = DataFeed::Ibkr;   // where ticks come from
    };
    using StartFn = std::function<void(const StartOpts&)>;

    // Active account shown in the panel header (from the gateway/store).
    struct AccountInfo {
        std::string label;   // e.g. "claudiagosselin"; empty = not signed in
        int kind = 0;        // 0 unknown, 1 paper, 2 live
        bool readonly = false;
        // Tradeable (sub-)accounts under the login; >1 shows a per-symbol picker.
        std::vector<std::string> subaccounts;
    };

    TradePanel(Engine& eng, QuoteBook& quotes) : eng_(eng), quotes_(quotes) {}
    // strat_sources: available strategy source basenames ("" built-in is added
    // in the UI). polygon_available / finnhub_available: a key for that vendor
    // exists right now. ibkr_ready: the IBKR gateway is connected, so orders
    // route to it; otherwise the local fill simulator is used.
    void draw(bool* open, const std::vector<std::string>& strat_sources,
              const ParamSpecsFn& strat_params, const StratNameFn& strat_name,
              const AutoPickFn& autopick, bool polygon_available, bool finnhub_available,
              bool ibkr_ready, const AccountInfo& account, const StartFn& start,
              const SymbolPickFn& chart_pick);

    // The session schedule — auto-start at sched_start_, kill-switch + stop at
    // sched_stop_ (local clock, weekdays). Driven from App::tick(), NOT from
    // draw().
    //
    // It used to live inside draw(), below `if (!ImGui::Begin("Trade", open))
    // return;`. ImGui::Begin reports false for a COLLAPSED window and for a
    // docked tab that is not the selected one — and App::setup_default_layout
    // docks Trade into the same node as Backtest and Strategy. So one click on
    // either neighbouring tab disabled both the 09:25 auto-start and the 15:55
    // auto-stop, on a maximized, fully visible window, with nothing anywhere to
    // say the schedule had stopped running. Minimizing killed it outright,
    // because the host loop skipped draw() entirely (2026-08-11).
    //
    // The scheduled STOP is the dangerous half: the engine's own 15:57 EOD
    // backstop flattens but does not halt or end the session, so a stop that
    // never fires leaves the session live overnight.
    void pump_schedule(const AccountInfo& account, const ParamSpecsFn& strat_params,
                       bool polygon_available, bool finnhub_available, bool ibkr_ready,
                       const StartFn& start);

    // Tournament champion: point this symbol's tab at a strategy + its params
    // (adds the tab if the symbol isn't pending yet).
    void set_symbol_strategy(const std::string& symbol, const std::string& key,
                             const std::map<std::string, double>& params);

    // Merge a fit into an EXISTING tab that already runs `key`; returns false
    // otherwise. A plain Optimizer sweep must not conjure a Trade tab for a
    // symbol you only meant to optimize, nor silently re-point a tab at the
    // strategy you were experimenting with — but its winner still belongs to
    // that symbol rather than to the shared per-strategy map (symbol_params.h).
    //
    // MERGE, not replace: the caller passes only the names its sweep actually
    // tuned. A sweep never tunes the sizing and session-shape knobs
    // (sweep_param_is_fixed skips qty, alloc_pct, risk_pct, enter_from_h, ...),
    // so its winner still carries the STRATEGY-WIDE value for each of those —
    // and replacing the whole map would quietly overwrite this symbol's own
    // sizing with another symbol's.
    bool merge_symbol_params(const std::string& symbol, const std::string& key,
                             const std::map<std::string, double>& params);

    // Lineup admission (symbol_params.h): the pending symbols, whether one
    // carries a parameter set of its own, and dropping the ones that earned no
    // validated set this morning. remove_symbols returns how many tabs went.
    std::vector<std::string> pending_symbols() const;
    // "Of its own" means: a value for at least one name the strategy this tab is
    // CURRENTLY set to declares. A bare !params.empty() test let a tab whose
    // strategy had been changed under its old params report true, get admitted
    // as AdmitOwnPrevious, and then resolve to ParamSource::Inherited in
    // build_start_opts — which drops every undeclared key. That is the exact
    // "trades a set that was never fitted to it" outcome this rule exists to
    // stop, reached through the admission rule meant to prevent it.
    bool has_own_params(const std::string& symbol,
                        const ParamSpecsFn& strat_params) const;
    int remove_symbols(const std::vector<std::string>& symbols);

    // Suppress the scheduled auto-start for the rest of today. The daily lineup
    // calls this when it refuses to start a session: the auto-start below is
    // level-triggered on (sched_on_, pending_, clock) alone and knows nothing
    // about the lineup, so "we started nothing" would otherwise be undone a
    // frame later by the scheduler starting those same tabs on the borrowed
    // parameters the lineup just rejected. The manual Start button still works —
    // this blocks the unattended path, not the operator.
    void block_scheduled_start();

    // Replace all pending tabs with exactly `symbols`, each a fresh tab seeded
    // from the panel's per-symbol defaults (built-in strategy until a
    // tournament assigns one). Used by the daily auto-lineup. No-op if empty
    // (keeps the current tabs rather than leaving the panel symbol-less).
    // A symbol that is already a tab keeps its (strategy, params) PAIR: if this
    // morning's tournament produces nothing for it, its own previous fit is the
    // only set left that was ever fitted to this instrument.
    void set_lineup(const std::vector<std::string>& symbols);

    // Build the StartOpts the "Start Trading" button would send, from the
    // current tabs + panel settings (also latches session_broker_ for the
    // running-session header). Exposed so the daily-lineup scheduler auto-starts
    // through the exact same construction — one start path, no duplication.
    StartOpts build_start_opts(const AccountInfo& account,
                               const ParamSpecsFn& strat_params, bool polygon_available,
                               bool finnhub_available, bool ibkr_ready);
    // Whether any symbol tab exists (the scheduler won't auto-start an empty set).
    bool has_symbols() const { return !pending_.empty(); }

    // Persisted: the shared cash pool + per-symbol defaults for new cards.
    double cash() const { return session_cash_; }
    int bar_sec() const { return def_bar_sec_; }
    int data_idx() const { return data_idx_; }
    int route() const { return route_; }
    bool record() const { return def_record_; }
    void restore(double cash, int bar_sec, int data_idx, bool record, int route) {
        session_cash_ = cash;
        def_bar_sec_ = bar_sec;
        data_idx_ = (data_idx >= 0 && data_idx <= 3) ? data_idx : 0;
        def_record_ = record;
        route_ = (route >= 0 && route <= 1) ? route : 0;
    }
    // Persisted session schedule: auto start/stop times ("HH:MM", local clock,
    // weekdays only). The stop flattens via the kill switch.
    bool sched_on() const { return sched_on_; }
    std::string sched_start() const { return sched_start_; }
    std::string sched_stop() const { return sched_stop_; }
    void restore_schedule(bool on, const std::string& start, const std::string& stop,
                          int blocked_day = -1);
    // tm_yday the lineup blocked the auto-start on; -1 = not blocked. Persisted
    // so a restart cannot resurrect a session the lineup refused.
    int sched_blocked_day() const { return sched_blocked_day_; }

    // Persisted default risk limits, seeded into each new symbol card.
    const RiskLimits& risk() const { return def_risk_; }
    double risk_dd_pct() const { return def_risk_dd_pct_; }
    void restore_risk(const RiskLimits& r, double dd_pct) {
        def_risk_ = r;
        def_risk_dd_pct_ = dd_pct;
    }

    // The symbol tabs + their per-symbol settings, for session persistence.
    std::vector<TradeSymbol> symbols_config() const;
    void restore_symbols(const std::vector<TradeSymbol>& syms);

private:
    Engine& eng_;
    QuoteBook& quotes_;
    int data_idx_ = 0;               // DataFeed enum (persisted)
    int route_ = 0;                  // order route: 0 Web API (auto), 1 TWS socket
    int session_broker_ = 0;         // what the running session was started with
    int want_tab_ = -1;              // tab-list button: symbol index to select
    char input_[16] = "";
    // One tab per pending symbol: its bar size, capture flag, sub-account (when
    // the login has them), and its own risk limits.
    struct SymRow {
        std::string symbol;
        int bar_sec = 60;
        bool record = true;
        int account_idx = 0;        // index into AccountInfo.subaccounts
        RiskLimits risk{};
        double risk_dd_pct = 0.0;   // UI percent; converted to fraction at start
        std::string strat_key;      // strategy source basename; "" = built-in SMA
        std::map<std::string, double> params;   // per-symbol strategy param edits
        int ap_mode = 0;            // autopilot: 0 off, 1 params, 2 full
        int ap_trigger = 0;         // 0 timer, 1 drawdown, 2 both
        double ap_interval_min = 30, ap_dd_pct = 5;
    };
    std::vector<SymRow> pending_ = {{"AAPL", 60, true, 0, {}, 0.0, "", {}, 0, 0, 30, 5}};
    // Shared-pool cash (simulator / single account) + per-symbol defaults.
    double session_cash_ = 100'000.0;
    int def_bar_sec_ = 60;
    bool def_record_ = true;
    RiskLimits def_risk_{};
    double def_risk_dd_pct_ = 0.0;   // UI shows percent; RiskLimits stores fraction
    std::string def_strat_key_;      // strategy seeded into the next added symbol
    int def_ap_mode_ = 0, def_ap_trigger_ = 0;
    double def_ap_interval_min_ = 30, def_ap_dd_pct_ = 5;
    double manual_qty_ = 10.0;
    double manual_tp_ = 0.0, manual_sl_ = 0.0;
    double manual_lmt_ = 0.0;   // 0 = auto marketable limit; >0 = that limit price
    int selected_symbol_idx_ = 0;

    // Session schedule (persisted: sched_on_ + the two "HH:MM" strings).
    bool sched_on_ = false;
    char sched_start_[8] = "09:25";
    char sched_stop_[8] = "15:55";
    int sched_last_start_day_ = -1;   // tm_yday: one auto-start per day
    int sched_prev_min_ = -1;         // edge detector for the scheduled stop
    // tm_yday the daily lineup refused to start on; the auto-start stands down
    // for that day (block_scheduled_start). Persisted with the schedule — a
    // restart inside the session window would otherwise re-enable it.
    int sched_blocked_day_ = -1;
};

} // namespace tt::ui
