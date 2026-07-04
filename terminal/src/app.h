#pragma once

#include "engine/builtin_sma.h"
#include "engine/engine.h"
#include "market_data.h"
#include "net/ipc_client.h"
#include "panels/backtest.h"
#include "panels/chart.h"
#include "panels/log_console.h"
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
    };
    void start_pending_backtest(net::CandleBatch& batch);

    LogConsole log_;
    SeriesStore series_;
    QuoteBook quotes_;
    net::IpcClient ipc_;
    Engine engine_;
    SmaCrossover sma_;
    ChartPanel chart_;
    WatchlistPanel watchlist_;
    BacktestPanel backtest_;

    std::mutex pending_bt_mu_;
    PendingBacktest pending_bt_;

    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool autorun_bt_done_ = false;
    bool show_chart_ = true;
    bool show_watchlist_ = true;
    bool show_backtest_ = true;
    bool show_log_ = true;
    bool show_imgui_demo_ = false;
    bool show_implot_demo_ = false;
};

} // namespace tt::ui
