#pragma once
// Session persistence: %LOCALAPPDATA%/TradeTerminal/config.json.
// Loaded at startup, saved on clean exit. Missing/corrupt files fall back to
// defaults silently — config must never block launch.

#include <string>
#include <vector>

namespace tt::ui {

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

    static AppConfig load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace tt::ui
