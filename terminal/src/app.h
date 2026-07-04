#pragma once

#include "config.h"
#include "engine/builtin_sma.h"
#include "engine/engine.h"
#include "engine/strategy_host.h"
#include "market_data.h"
#include "net/ipc_client.h"
#include "panels/backtest.h"
#include "panels/blotter.h"
#include "panels/chart.h"
#include "panels/log_console.h"
#include "panels/positions.h"
#include "panels/strategy_mgr.h"
#include "panels/trade.h"
#include "panels/watchlist.h"

#include "imgui.h"

#include <map>
#include <mutex>
#include <string>

namespace tt::ui {

// Owns the data stores, the sidecar connection, and the panels; draws one
// frame of UI inside the dockspace.
class App {
public:
    App(std::string python_cmd, std::string service_dir);
    ~App();

    void draw();

    // Whether an imgui.ini existed at startup; if not, a default dock layout
    // is built on the first frame.
    void set_had_ini(bool v) { had_ini_ = v; }
    LogConsole& log() { return log_; }

private:
    void draw_menu_bar();
    void setup_default_layout(ImGuiID dockspace_id);

    // Set on the UI thread when Run is clicked; consumed on the IPC thread
    // when the matching candle response arrives.
    struct PendingBacktest {
        bool active = false;
        std::string symbol, interval;
        std::map<std::string, double> params;
        double cash = 0.0;
        IStrategy* strategy = nullptr;   // captured at click time
        uint64_t strategy_gen = 0;       // host generation at click time
    };
    void start_pending_backtest(net::CandleBatch& batch);
    void queue_backtest(const std::string& sym, const std::string& ivl,
                        const std::string& rng, double cash);

    LogConsole log_;
    SeriesStore series_;
    QuoteBook quotes_;
    net::IpcClient ipc_;
    Engine engine_;
    SmaCrossover sma_;
    StrategyHost host_;
    ChartPanel chart_;
    WatchlistPanel watchlist_;
    BacktestPanel backtest_;
    StrategyManagerPanel strat_mgr_;
    TradePanel trade_;
    BlotterPanel blotter_;
    PositionsPanel positions_;

    std::mutex pending_bt_mu_;
    PendingBacktest pending_bt_;

    std::string config_path_;
    AppConfig cfg_;

    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool autorun_bt_done_ = false;
    bool show_chart_ = true;
    bool show_watchlist_ = true;
    bool show_backtest_ = true;
    bool show_strategy_ = true;
    bool show_trade_ = true;
    bool show_blotter_ = true;
    bool show_positions_ = true;
    bool show_log_ = true;
    bool show_imgui_demo_ = false;
    bool show_implot_demo_ = false;
    int autorun_live_stage_ = 0;
    double sim_tick_next_s_ = 0.0;
    double sim_tick_px_ = 0.0;
    unsigned sim_tick_rng_ = 0x5eed;
};

} // namespace tt::ui
