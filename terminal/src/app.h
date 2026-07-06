#pragma once

#include "account_store.h"
#include "alerts.h"
#include "config.h"
#include "engine/ibkr_broker.h"
#include "engine/ibkr_feed.h"
#include "engine/polygon_feed.h"
#include "engine/builtin_sma.h"
#include "engine/engine.h"
#include "engine/strategy_host.h"
#include "journal.h"
#include "market_data.h"
#include "net/gateway_data.h"
#include "panels/backtest.h"
#include "panels/blotter.h"
#include "panels/chart.h"
#include "panels/journal_panel.h"
#include "panels/log_console.h"
#include "panels/positions.h"
#include "panels/strategy_mgr.h"
#include "panels/sweep.h"
#include "panels/trade.h"
#include "panels/watchlist.h"

#include "imgui.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace tt::ui {

// Owns the data stores, the sidecar connection, and the panels; draws one
// frame of UI inside the dockspace.
class App {
public:
    explicit App(std::string gateway_url);
    ~App();

    void draw();

    // Whether an imgui.ini existed at startup; if not, a default dock layout
    // is built on the first frame.
    void set_had_ini(bool v) { had_ini_ = v; }
    LogConsole& log() { return log_; }

private:
    void draw_menu_bar();
    void draw_account_menu();
    void draw_signin_modal();
    void alert_scan(const std::string& log_line);
    void setup_default_layout(ImGuiID dockspace_id);
    // Signed-in Polygon key, falling back to POLYGON_API_KEY; "" = none.
    std::string polygon_key() const;

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

    // ---- parameter sweep (all state UI-thread only unless noted) ----
    void queue_sweep(const SweepPanel::Request& rq);
    void stash_pending_sweep(net::CandleBatch& batch);   // IPC thread
    void pump_sweep();                                   // UI thread, per frame
    void start_sweep_cell();

    LogConsole log_;
    SeriesStore series_;
    QuoteBook quotes_;
    net::GatewayData gw_;
    // Declared before engine_ on purpose: the engine's live thread holds a
    // raw pointer to the broker, so the broker must be destroyed after the
    // engine (members destruct in reverse declaration order).
    std::unique_ptr<IbkrBroker> ibkr_;
    Engine engine_;
    // Declared after engine_ on purpose: the feed pushes into the engine's
    // ring, so it must be destroyed (thread joined) before the engine.
    std::unique_ptr<PolygonFeed> polygon_feed_;
    std::unique_ptr<IbkrFeed> ibkr_feed_;
    std::atomic<bool> rt_feed_active_{false};   // worker thread: skip snapshot ticks
    AlertNotifier alerts_;
    SmaCrossover sma_;
    StrategyHost host_;
    ChartPanel chart_;
    WatchlistPanel watchlist_;
    BacktestPanel backtest_;
    StrategyManagerPanel strat_mgr_;
    TradePanel trade_;
    BlotterPanel blotter_;
    PositionsPanel positions_;
    SweepPanel sweep_panel_;

    // ---- trade journal (UI thread only) ----
    TradeJournal journal_;
    JournalPanel journal_panel_{journal_};
    int64_t journal_session_ = 0;          // 0 = no open session row
    std::vector<std::string> journal_syms_;
    bool prev_live_running_ = false;

    // Sweep runner. The IPC thread only stashes fetched candles under
    // pending_bt_mu_; everything else runs on the UI thread.
    struct SweepSetup {
        bool ready = false, waiting = false;
        SweepPanel::Request req;
        std::vector<Bar> bars;
        IStrategy* strategy = nullptr;
        uint64_t strategy_gen = 0;
    };
    SweepSetup sweep_setup_;
    SweepPanel::State sweep_;
    BacktestConfig sweep_base_;          // train slice
    std::vector<Bar> sweep_test_bars_;   // holdout slice (never optimized on)
    bool sweep_holdout_phase_ = false;
    IStrategy* sweep_strategy_ = nullptr;
    uint64_t sweep_gen_ = 0;

    std::mutex pending_bt_mu_;
    PendingBacktest pending_bt_;

    // ---- Account menu / Sign In modal ----
    AccountStore accounts_;
    struct SignIn {
        bool request_open = false;        // menu clicked; OpenPopup next frame
        int provider = 0;                 // 0 alpaca, 1 polygon, 2 ibkr (info only)
        char name[32] = "paper";
        char key[96] = "";
        char secret[128] = "";
        // 0 idle, 1 verifying, 2 verified-ok (consume + save), 3 failed
        std::atomic<int> status{0};
        std::string detail;               // guarded by mu_
        std::mutex mu;
        std::thread worker;               // joined before reuse and in ~App
    } signin_;

    std::string config_path_;
    AppConfig cfg_;

    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool autorun_bt_done_ = false;
    bool autorun_sweep_done_ = false;
    bool show_chart_ = true;
    bool show_watchlist_ = true;
    bool show_backtest_ = true;
    bool show_sweep_ = false;
    bool show_strategy_ = true;
    bool show_trade_ = true;
    bool show_blotter_ = true;
    bool show_positions_ = true;
    bool show_journal_ = false;
    bool show_log_ = true;
    bool show_imgui_demo_ = false;
    bool show_implot_demo_ = false;
    int autorun_live_stage_ = 0;
#ifdef TT_DEBUG
    bool sim_ticks_ = false;   // Debug menu toggle; TT_SIM_TICKS=1 pre-enables
    double sim_tick_next_s_ = 0.0;
    double sim_tick_px_ = 0.0;
    unsigned sim_tick_rng_ = 0x5eed;
#endif
};

} // namespace tt::ui
