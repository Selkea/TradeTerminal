#pragma once

#include "account_store.h"
#include "alerts.h"
#include "config.h"
#include "engine/ibkr_broker.h"
#include "engine/ibkr_feed.h"
#include "engine/polygon_feed.h"
#include "engine/finnhub_feed.h"
#include "engine/builtin_sma.h"
#include "engine/engine.h"
#include "engine/strategy_host.h"
#include "journal.h"
#include "market_data.h"
#include "net/gateway_data.h"
#include "panels/backtest.h"
#include "panels/blotter.h"
#include "panels/replay.h"
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

    // Window-close request from the host loop. Quits immediately if nothing is
    // trading; otherwise pops a confirm dialog and quits only once confirmed.
    void request_quit();
    bool should_quit() const { return should_quit_; }

    // Whether an imgui.ini existed at startup; if not, a default dock layout
    // is built on the first frame.
    void set_had_ini(bool v) { had_ini_ = v; }
    LogConsole& log() { return log_; }

private:
    void draw_menu_bar();
    void draw_account_menu();          // broker (IBKR) menu
    void draw_data_menu();             // data-feed (Polygon/Finnhub) menu
    void draw_account_modal();         // broker sign-in / switch dialog
    void draw_data_modal();            // data-feed sign-in dialog
    void draw_trading_guards();        // Sign Out / quit confirm dialogs
    void safe_stop_live();            // kill switch + graceful stop, if live
    void do_ibkr_signout();          // run Stop-IbkrLogin, log
    void refresh_ibkr_accounts();     // reload labels from ibkr-accounts.json
    void alert_scan(const std::string& log_line);
    void setup_default_layout(ImGuiID dockspace_id);
    // Signed-in Polygon key, falling back to POLYGON_API_KEY; "" = none.
    std::string polygon_key() const;
    // Signed-in Finnhub key, falling back to FINNHUB_API_KEY; "" = none.
    std::string finnhub_key() const;

    // Set on the UI thread when Run is clicked; consumed on the IPC thread
    // when the matching candle response arrives.
    struct PendingBacktest {
        bool active = false;
        std::string symbol, interval;
        std::map<std::string, double> params;
        double cash = 0.0;
        IStrategy* strategy = nullptr;   // leased instance, captured at click
    };
    void start_pending_backtest(net::CandleBatch& batch);
    void queue_backtest(const std::string& key, const std::string& sym,
                        const std::string& ivl, const std::string& rng,
                        double cash);

    // ---- per-run strategy instances ----
    // Every run (backtest, sweep, live, replay) gets its own instance; a
    // lease tracks it until the run can no longer touch the pointer, then
    // pump_leases() destroys it (host for DLLs, destroy() for the built-in).
    struct StrategyLease {
        IStrategy* inst = nullptr;
        std::string key;                 // "" = built-in SMA
        enum Kind { Backtest, Sweep, Live } kind = Backtest;
    };
    std::vector<StrategyLease> leases_;
    IStrategy* acquire_strategy(const std::string& key);
    void release_strategy(const StrategyLease& lease);
    void pump_leases();                  // UI thread, per frame

    // Backtest-panel dropdown picked a strategy that may not be loaded:
    // build + load it via the Strategy Manager machinery, then run. All on
    // the UI thread.
    struct PendingStrategyRun {
        bool active = false;
        std::string src, symbol, interval, range;
        double cash = 0.0;
    };
    PendingStrategyRun pending_run_;
    void queue_backtest_as(const std::string& src, const std::string& sym,
                           const std::string& ivl, const std::string& rng,
                           double cash);
    void pump_pending_run();   // UI thread, per frame

    // ---- parameter sweep (all state UI-thread only unless noted) ----
    void queue_sweep(const SweepPanel::Request& rq);
    void stash_pending_sweep(net::CandleBatch& batch);   // IPC thread
    void pump_sweep();                                   // UI thread, per frame
    void start_sweep_cell();
    void start_opt_param();   // begin the current param's 1-D sweep

    // ---- strategy tournament (auto-pick) ----
    // Runs the optimizer once per candidate strategy on the same data, crowns
    // the best holdout score, applies the champion to a Trade-tab symbol.
    struct Tournament {
        enum class Phase { Launch, Queued, Running };
        bool active = false;
        Phase phase = Phase::Launch;
        std::vector<std::string> candidates;   // "" = built-in
        size_t idx = 0;
        struct Entry {
            std::string key;
            std::map<std::string, double> params;
            double score = 0;
            bool holdout = false;   // score came from unseen data
            bool valid = false;
        };
        std::vector<Entry> results;
        SweepPanel::Request base;
        std::string target_symbol;   // Trade tab row the champion applies to
        double stamp_s = 0;          // phase-entry time, for timeouts
    };
    Tournament tourn_;
    // candidates: explicit strategy keys to race; empty = built-in + all loaded.
    void start_tournament(SweepPanel::Request rq, const std::string& target_symbol,
                          std::vector<std::string> candidates = {});
    void pump_tournament();      // UI thread, per frame
    void finish_tournament();

    // ---- autopilot: re-optimize symbols while the live session trades ----
    // Cycles run through the tournament machinery (params mode = a one-
    // candidate tournament of the incumbent); results apply to the LIVE
    // session via the engine's flat-only hot-swap, guarded by hysteresis and,
    // for strategy swaps, a two-consecutive-wins streak.
    struct Autopilot {
        struct Sym {
            std::string symbol;
            uint32_t sid = 0;
            int mode = 0;              // 0 off, 1 params, 2 full
            int trigger = 0;           // 0 timer, 1 drawdown, 2 both
            double interval_min = 30, dd_pct = 5;
            std::string key;           // incumbent strategy
            double last_cycle_s = 0;
            double incumbent_score = 0;
            bool has_score = false;
            std::string challenger;    // full mode: pending challenger + streak
            int streak = 0;
        };
        std::vector<Sym> syms;
        int in_flight = -1;            // index into syms; -1 = idle
        int metric = 0;                // cycle's scoring metric
        double session_high_eq = 0;
        double last_dd_cycle_s = 0;    // drawdown-trigger cooldown
    };
    Autopilot ap_;
    void pump_autopilot();       // UI thread, per frame
    void autopilot_evaluate();   // consume a finished cycle's tournament result

    // Coordinate-descent state for the auto-optimizer (UI thread only).
    struct AutoOpt {
        struct Param {
            std::string name;
            double min = 0, max = 0;
        };
        std::vector<Param> params;
        std::map<std::string, double> best;   // best values so far
        std::string key;                      // strategy being optimized
        int pass = 0;
        int pi = 0;                           // index into params
        int step = 0;                         // index into sweep_.xs
        double best_metric = 0;
        bool metric_valid = false;
    };
    AutoOpt opt_;

    LogConsole log_;
    SeriesStore series_;
    QuoteBook quotes_;
    net::GatewayData gw_;
    // Declared before engine_ on purpose: the engine's live thread holds a
    // raw pointer to the broker, so the broker must be destroyed after the
    // engine (members destruct in reverse declaration order).
    std::unique_ptr<IbkrBroker> ibkr_;
    // Same reasoning: the host destroys any leftover per-run strategy
    // instances and unloads their DLLs, which must happen only after the
    // engine's threads are joined.
    StrategyHost host_;
    Engine engine_;
    // Declared after engine_ on purpose: the feed pushes into the engine's
    // ring, so it must be destroyed (thread joined) before the engine.
    std::unique_ptr<PolygonFeed> polygon_feed_;
    std::unique_ptr<FinnhubFeed> finnhub_feed_;
    std::unique_ptr<IbkrFeed> ibkr_feed_;
    std::atomic<bool> rt_feed_active_{false};   // worker thread: skip snapshot ticks
    AlertNotifier alerts_;
    ChartPanel chart_;
    WatchlistPanel watchlist_;
    BacktestPanel backtest_;
    ReplayPanel replay_;
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
        IStrategy* strategy = nullptr;   // leased instance
        std::string key;                 // strategy key, for the label
        std::map<std::string, double> params;   // captured at queue time
    };
    SweepSetup sweep_setup_;
    SweepPanel::State sweep_;
    BacktestConfig sweep_base_;          // train slice
    std::vector<Bar> sweep_test_bars_;   // holdout slice (never optimized on)
    bool sweep_holdout_phase_ = false;
    IStrategy* sweep_strategy_ = nullptr;

    std::mutex pending_bt_mu_;
    PendingBacktest pending_bt_;

    // ---- Account menu / Sign In modal ----
    AccountStore accounts_;
    struct SignIn {
        // Two separate dialogs: Account (broker/IBKR) and Data (feeds).
        bool account_request_open = false;   // Account menu clicked; OpenPopup next frame
        bool data_request_open = false;      // Data menu clicked
        bool account_open = true;            // Account modal p_open (title-bar X)
        bool data_open = true;               // Data modal p_open
        int broker_provider = 0;             // Account dialog: 0 IBKR (room for more brokers)
        int provider = 0;                    // Data dialog: 0 Polygon, 1 Finnhub
        char name[32] = "paper";
        char key[96] = "";
        char secret[128] = "";
        // 0 idle, 1 verifying, 2 verified-ok (consume + save), 3 failed
        std::atomic<int> status{0};
        std::string detail;               // guarded by mu_
        std::mutex mu;
        std::thread worker;               // joined before reuse and in ~App
        // IBKR account picker (from ibkr-accounts.json; UI thread only)
        std::vector<std::string> ibkr_accounts;     // unique keys (switch/remove)
        std::vector<std::string> ibkr_labels;       // parallel: display names
        std::vector<unsigned char> ibkr_paper;      // parallel: 1 = paper, 0 = live
        std::vector<unsigned char> ibkr_readonly;   // parallel: 1 = trading disabled
        std::string ibkr_active;
        int ibkr_selected = -1;
        std::string pending_account;   // account awaiting the "live trading" switch confirm
    } signin_;

    // While the gateway is being launched, show "INITIALIZING" until it is up.
    // Holds an ImGui::GetTime() deadline; 0 = not starting.
    double gateway_starting_until_ = 0.0;

    std::string config_path_;
    AppConfig cfg_;

    bool should_quit_ = false;        // host loop exits when true
    bool pending_quit_ = false;       // quit awaiting the "live trading" confirm
    bool pending_signout_ = false;    // sign-out awaiting the "live trading" confirm

    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool autorun_bt_done_ = false;
    bool autorun_sweep_done_ = false;
    bool show_chart_ = true;
    bool show_watchlist_ = true;
    bool show_backtest_ = true;
    bool show_replay_ = false;
    bool show_sweep_ = false;
    bool show_strategy_ = true;
    bool show_build_output_ = true;
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
