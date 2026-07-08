#pragma once
// Session persistence: %LOCALAPPDATA%/TradeTerminal/config.json.
// Loaded at startup, saved on clean exit. Missing/corrupt files fall back to
// defaults silently — config must never block launch.

#include <map>
#include <string>
#include <vector>

namespace tt::ui {

// One Trade-panel symbol tab, persisted so the session's symbols + their
// strategies/settings survive a restart.
struct TradeSymbol {
    std::string symbol;
    int bar_sec = 60;
    bool record = true;
    std::string strat_key;          // "" = built-in SMA
    int account_idx = 0;            // index into the login's sub-accounts
    double risk_max_order_qty = 1'000;
    double risk_max_position_qty = 5'000;
    double risk_daily_max_loss = 0;
    int risk_stale_feed_sec = 0;
    double risk_dd_pct = 0;         // percent, as shown in the UI
    std::map<std::string, double> params;   // this symbol's strategy params
    // Autopilot (re-optimize while trading): mode 0 off / 1 params / 2 full;
    // trigger 0 timer / 1 drawdown / 2 both.
    int ap_mode = 0;
    int ap_trigger = 0;
    double ap_interval_min = 30;
    double ap_dd_pct = 5;
};

struct AppConfig {
    std::vector<std::string> watchlist = {"AAPL", "MSFT", "SPY"};
    std::string chart_symbol = "AAPL";
    int chart_interval_idx = 1;
    int chart_range_idx = 1;
    double backtest_cash = 100'000.0;
    double trade_cash = 100'000.0;
    int trade_bar_sec = 60;
    int trade_data_idx = 0;      // last data-feed pick (DataFeed enum)
    bool trade_record = true;    // last "Record" toggle
    // Plain-text POST target for fill/halt/disconnect alerts (ntfy.sh-style);
    // TT_ALERT_WEBHOOK env var overrides. Empty = beeps only.
    std::string alert_webhook;
    // Command that starts the IBKR Client Portal Gateway (e.g. its run.bat
    // with the conf path); enables the Launch button in Sign In. Empty = off.
    std::string ibkr_gateway_cmd;
    // Risk limits survive restarts — a halt threshold you set once should
    // still be armed tomorrow.
    double risk_max_order_qty = 1'000;
    double risk_max_position_qty = 5'000;
    double risk_daily_max_loss = 0;
    double risk_max_drawdown_pct = 0;   // percent, as shown in the UI
    int risk_stale_feed_sec = 0;

    // Trade panel: the symbol tabs from last session (empty = default AAPL).
    std::vector<TradeSymbol> trade_symbols;
    // Backtest panel: last strategy pick ("" = built-in).
    std::string backtest_strategy;
    // Replay panel: last strategy pick, cash, and re-bar override.
    std::string replay_strategy;
    double replay_cash = 100'000.0;
    int replay_bar_sec = 0;
    // Optimizer panel settings.
    std::string sweep_strategy;
    std::string sweep_symbol = "AAPL";
    int sweep_interval_idx = 2;
    int sweep_range_idx = 3;
    double sweep_cash = 100'000.0;
    int sweep_metric = 0;
    bool sweep_holdout = true;
    double sweep_holdout_pct = 25.0;
    // Which panels (View menu) were open; missing entry = the panel's default.
    std::map<std::string, bool> panels;
    // Strategy panel: which strategies were loaded and each one's edited
    // parameter values — restored (rebuilt) on startup.
    std::vector<std::string> strategy_loaded;       // .cpp basenames to reload
    std::map<std::string, std::map<std::string, double>> strategy_params;

    static AppConfig load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace tt::ui
