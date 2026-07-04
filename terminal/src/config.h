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

    static AppConfig load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace tt::ui
