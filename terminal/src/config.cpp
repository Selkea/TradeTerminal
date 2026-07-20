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
    c.trade_data_idx = j.value("trade_data_idx", c.trade_data_idx);
    c.trade_record = j.value("trade_record", c.trade_record);
    c.trade_route = j.value("trade_route", c.trade_route);
    c.trade_sched_on = j.value("trade_sched_on", c.trade_sched_on);
    c.trade_sched_start = j.value("trade_sched_start", c.trade_sched_start);
    c.trade_sched_stop = j.value("trade_sched_stop", c.trade_sched_stop);
    c.alert_webhook = j.value("alert_webhook", c.alert_webhook);
    c.ibkr_gateway_cmd = j.value("ibkr_gateway_cmd", c.ibkr_gateway_cmd);
    c.risk_max_order_qty = j.value("risk_max_order_qty", c.risk_max_order_qty);
    c.risk_max_position_qty = j.value("risk_max_position_qty", c.risk_max_position_qty);
    c.risk_daily_max_loss = j.value("risk_daily_max_loss", c.risk_daily_max_loss);
    c.risk_max_drawdown_pct = j.value("risk_max_drawdown_pct", c.risk_max_drawdown_pct);
    c.risk_stale_feed_sec = j.value("risk_stale_feed_sec", c.risk_stale_feed_sec);

    if (j.contains("trade_symbols") && j["trade_symbols"].is_array())
        for (const auto& s : j["trade_symbols"]) {
            if (!s.is_object()) continue;
            TradeSymbol ts;
            ts.symbol = s.value("symbol", std::string());
            if (ts.symbol.empty()) continue;
            ts.bar_sec = s.value("bar_sec", ts.bar_sec);
            ts.record = s.value("record", ts.record);
            ts.strat_key = s.value("strat_key", ts.strat_key);
            ts.account_idx = s.value("account_idx", ts.account_idx);
            ts.risk_max_order_qty = s.value("risk_max_order_qty", ts.risk_max_order_qty);
            ts.risk_max_position_qty =
                s.value("risk_max_position_qty", ts.risk_max_position_qty);
            ts.risk_daily_max_loss = s.value("risk_daily_max_loss", ts.risk_daily_max_loss);
            ts.risk_stale_feed_sec = s.value("risk_stale_feed_sec", ts.risk_stale_feed_sec);
            ts.risk_dd_pct = s.value("risk_dd_pct", ts.risk_dd_pct);
            ts.ap_mode = s.value("ap_mode", ts.ap_mode);
            ts.ap_trigger = s.value("ap_trigger", ts.ap_trigger);
            ts.ap_interval_min = s.value("ap_interval_min", ts.ap_interval_min);
            ts.ap_dd_pct = s.value("ap_dd_pct", ts.ap_dd_pct);
            if (s.contains("params") && s["params"].is_object())
                for (const auto& [k, v] : s["params"].items())
                    if (v.is_number()) ts.params[k] = v.get<double>();
            c.trade_symbols.push_back(std::move(ts));
        }
    c.backtest_strategy = j.value("backtest_strategy", c.backtest_strategy);
    c.replay_strategy = j.value("replay_strategy", c.replay_strategy);
    c.replay_cash = j.value("replay_cash", c.replay_cash);
    c.replay_bar_sec = j.value("replay_bar_sec", c.replay_bar_sec);
    c.sweep_strategy = j.value("sweep_strategy", c.sweep_strategy);
    c.sweep_symbol = j.value("sweep_symbol", c.sweep_symbol);
    c.sweep_interval_idx = j.value("sweep_interval_idx", c.sweep_interval_idx);
    c.sweep_range_idx = j.value("sweep_range_idx", c.sweep_range_idx);
    c.sweep_cash = j.value("sweep_cash", c.sweep_cash);
    c.sweep_metric = j.value("sweep_metric", c.sweep_metric);
    c.sweep_holdout = j.value("sweep_holdout", c.sweep_holdout);
    c.sweep_holdout_pct = j.value("sweep_holdout_pct", c.sweep_holdout_pct);
    c.measured_lat_ns = j.value("measured_lat_ns", c.measured_lat_ns);
    c.measured_lat_jitter_ns = j.value("measured_lat_jitter_ns", c.measured_lat_jitter_ns);
    c.measured_lat_count = j.value("measured_lat_count", c.measured_lat_count);
    if (j.contains("panels") && j["panels"].is_object())
        for (const auto& [k, v] : j["panels"].items())
            if (v.is_boolean()) c.panels[k] = v.get<bool>();
    if (j.contains("strategy_loaded") && j["strategy_loaded"].is_array())
        for (const auto& s : j["strategy_loaded"])
            if (s.is_string()) c.strategy_loaded.push_back(s.get<std::string>());
    if (j.contains("strategy_params") && j["strategy_params"].is_object())
        for (const auto& [key, pv] : j["strategy_params"].items())
            if (pv.is_object())
                for (const auto& [k, v] : pv.items())
                    if (v.is_number()) c.strategy_params[key][k] = v.get<double>();
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
        {"trade_data_idx", trade_data_idx},
        {"trade_record", trade_record},
        {"trade_route", trade_route},
        {"trade_sched_on", trade_sched_on},
        {"trade_sched_start", trade_sched_start},
        {"trade_sched_stop", trade_sched_stop},
        {"alert_webhook", alert_webhook},
        {"ibkr_gateway_cmd", ibkr_gateway_cmd},
        {"risk_max_order_qty", risk_max_order_qty},
        {"risk_max_position_qty", risk_max_position_qty},
        {"risk_daily_max_loss", risk_daily_max_loss},
        {"risk_max_drawdown_pct", risk_max_drawdown_pct},
        {"risk_stale_feed_sec", risk_stale_feed_sec},
    };
    json syms = json::array();
    for (const auto& ts : trade_symbols)
        syms.push_back({{"symbol", ts.symbol},
                        {"bar_sec", ts.bar_sec},
                        {"record", ts.record},
                        {"strat_key", ts.strat_key},
                        {"account_idx", ts.account_idx},
                        {"risk_max_order_qty", ts.risk_max_order_qty},
                        {"risk_max_position_qty", ts.risk_max_position_qty},
                        {"risk_daily_max_loss", ts.risk_daily_max_loss},
                        {"risk_stale_feed_sec", ts.risk_stale_feed_sec},
                        {"risk_dd_pct", ts.risk_dd_pct},
                        {"params", ts.params},
                        {"ap_mode", ts.ap_mode},
                        {"ap_trigger", ts.ap_trigger},
                        {"ap_interval_min", ts.ap_interval_min},
                        {"ap_dd_pct", ts.ap_dd_pct}});
    j["trade_symbols"] = std::move(syms);
    j["backtest_strategy"] = backtest_strategy;
    j["replay_strategy"] = replay_strategy;
    j["replay_cash"] = replay_cash;
    j["replay_bar_sec"] = replay_bar_sec;
    j["sweep_strategy"] = sweep_strategy;
    j["sweep_symbol"] = sweep_symbol;
    j["sweep_interval_idx"] = sweep_interval_idx;
    j["sweep_range_idx"] = sweep_range_idx;
    j["sweep_cash"] = sweep_cash;
    j["sweep_metric"] = sweep_metric;
    j["sweep_holdout"] = sweep_holdout;
    j["sweep_holdout_pct"] = sweep_holdout_pct;
    j["measured_lat_ns"] = measured_lat_ns;
    j["measured_lat_jitter_ns"] = measured_lat_jitter_ns;
    j["measured_lat_count"] = measured_lat_count;
    j["panels"] = panels;
    j["strategy_loaded"] = strategy_loaded;
    j["strategy_params"] = strategy_params;

    std::ofstream f(path);
    if (f) f << j.dump(2);
}

} // namespace tt::ui
