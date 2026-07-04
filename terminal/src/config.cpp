#include "config.h"

#include "nlohmann/json.hpp"

#include <fstream>

using json = nlohmann::json;

namespace tt::ui {

AppConfig AppConfig::load(const std::string& path) {
    AppConfig c;
    std::ifstream f(path);
    if (!f) return c;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return c;

    if (j.contains("watchlist") && j["watchlist"].is_array()) {
        c.watchlist.clear();
        for (const auto& s : j["watchlist"])
            if (s.is_string()) c.watchlist.push_back(s.get<std::string>());
    }
    c.chart_symbol = j.value("chart_symbol", c.chart_symbol);
    c.chart_interval_idx = j.value("chart_interval_idx", c.chart_interval_idx);
    c.chart_range_idx = j.value("chart_range_idx", c.chart_range_idx);
    c.backtest_cash = j.value("backtest_cash", c.backtest_cash);
    c.trade_cash = j.value("trade_cash", c.trade_cash);
    c.trade_bar_sec = j.value("trade_bar_sec", c.trade_bar_sec);
    return c;
}

void AppConfig::save(const std::string& path) const {
    json j = {
        {"watchlist", watchlist},
        {"chart_symbol", chart_symbol},
        {"chart_interval_idx", chart_interval_idx},
        {"chart_range_idx", chart_range_idx},
        {"backtest_cash", backtest_cash},
        {"trade_cash", trade_cash},
        {"trade_bar_sec", trade_bar_sec},
    };
    std::ofstream f(path);
    if (f) f << j.dump(2);
}

} // namespace tt::ui
