#pragma once
// Session persistence: %LOCALAPPDATA%/TradeTerminal/config.json.
// Loaded at startup, saved on clean exit. Missing/corrupt files fall back to
// defaults silently — config must never block launch.

#include <cstdint>
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
    bool risk_disable_halt = false; // hold positions instead of halting on equity limits
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
    int trade_route = 0;         // order route: 0 IBKR web API, 1 TWS socket
    // Session schedule: auto start/stop ("HH:MM", local clock, weekdays).
    bool trade_sched_on = false;
    std::string trade_sched_start = "09:25";
    std::string trade_sched_stop = "15:55";

    // Daily auto-lineup (IBKR scan -> volatility rank -> tournament). The scan
    // and rank knobs feed the manual "Build today's lineup"; the schedule
    // fields drive the automated pre-market build (stage 3 wiring).
    bool lineup_enabled = false;              // auto-build on the session schedule
    std::string lineup_build_time = "09:35";  // HH:MM local; after the open so the
                                              // volatility ranking sees real range
    bool lineup_propose_only = true;          // build + log picks, do NOT auto-start
    std::string lineup_scan_code = "MOST_ACTIVE";
    std::string lineup_location = "STK.US.MAJOR";
    int lineup_rows = 30;                     // scan candidate pool size
    double lineup_min_price = 5.0;            // price gate (last close)
    double lineup_min_dollar_vol = 20e6;      // liquidity gate ($/day)
    int lineup_top_n = 6;                     // how many symbols to trade
    int lineup_atr_len = 14;                  // ATR window for the vol ranking
    // (The daily TWS refresh / tws_refresh_time setting was removed after it
    // caused two multi-hour gateway outages; see App::pump_tws_refresh's former
    // home in app.cpp and [[tt-tws-reconnect-freeze]]. Unknown keys in an older
    // config.json are ignored on load, so no migration is needed.)
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
    bool risk_disable_halt = false;     // default for new symbols: hold, don't halt

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
    // Order-path latency measured from the live broker's submit->ack round-trips
    // (median + p90-p50 spread, ns), fed into backtest/optimizer fills so they
    // model the real venue path instead of the 250 us default. count 0 = never
    // measured -> the sim falls back to the realistic VPS default (~75 ms).
    int64_t measured_lat_ns = 0;
    int64_t measured_lat_jitter_ns = 0;
    int64_t measured_lat_count = 0;
    // Read-only diagnostics HTTP endpoint for remote monitoring over the tailnet
    // (see net/diag_server.h). Bound to diag_bind:diag_port, guarded by a bearer
    // token auto-generated on first run (empty -> generated + persisted, and
    // printed once to the log). diag_enabled = false stops the server starting.
    bool diag_enabled = true;
    std::string diag_bind = "0.0.0.0";   // tailnet peers reach it; VPS firewall keeps it off the public net
    int diag_port = 8787;
    std::string diag_token;
    // Remote control (POST /control/kill -> engine kill switch: cancel all +
    // flatten + halt). OFF by default — the endpoint stays read-only until you
    // set this true. Uses a SEPARATE token so a leaked monitoring URL can never
    // flatten the book; auto-generated when control is first enabled.
    bool diag_control_enabled = false;
    std::string diag_control_token;
    // Which panels (View menu) were open; missing entry = the panel's default.
    std::map<std::string, bool> panels;
    // Strategy panel: which strategies were loaded and each one's edited
    // parameter values — restored (rebuilt) on startup.
    std::vector<std::string> strategy_loaded;       // .cpp basenames to reload
    std::map<std::string, std::map<std::string, double>> strategy_params;
    // Strategies the user has excluded from tournaments / auto-pick (keys;
    // "" = built-in). A tournament skips these when it uses the default
    // (all-loaded) candidate set.
    std::vector<std::string> strategy_tourn_excluded;

    static AppConfig load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace tt::ui
