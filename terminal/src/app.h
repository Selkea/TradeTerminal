#pragma once

#include "account_store.h"
#include "alerts.h"
#include "config.h"
#include "engine/ibkr_broker.h"
#include "engine/ibkr_feed.h"
#include "engine/polygon_feed.h"
#include "engine/finnhub_feed.h"
#include "engine/tws_broker.h"
#include "engine/tws_feed.h"
#include "engine/engine.h"
#include "engine/strategy_host.h"
#include "engine/symbol_rank.h"
#include "tt/strategy_registry.h"
#include "journal.h"
#include "market_data.h"
#include "net/diag_server.h"
#include "net/gateway_data.h"
#include "net/tws_data.h"
#include "update_check.h"
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
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
    void draw_update_panel();          // "update available" panel + confirm
    void launch_updater();             // hand off to Update-And-Restart.ps1, then quit
    // Graceful stop of a live session. keep_positions: on the reconciling (TWS)
    // route, skip the flatten so open positions + resting orders stay at the
    // broker to be re-adopted on restart; ignored (flattens) on other routes.
    void safe_stop_live(bool keep_positions = false);
    void do_ibkr_signout();          // run Stop-IbkrLogin, log
    void save_config();              // panel state -> cfg_ -> config.json
    // Read-only diagnostics endpoint (net/diag_server.h). start_diag_server()
    // generates/persists the bearer token and binds the socket; pump_diag()
    // re-renders the /diag body on the UI thread (throttled) into diag_json_,
    // which the server thread copies out under diag_mu_.
    void start_diag_server();
    void pump_diag();
    std::string build_diag_json();
    std::string build_metrics();                   // Prometheus exposition text
    std::string build_logs_json(uint64_t since);   // server thread (LogConsole is thread-safe)
    std::string build_logs_sse(uint64_t& cursor);  // server (stream) thread; advances cursor
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

    // ---- daily auto-lineup: scan -> rank -> tournament -> populate tabs ----
    // Builds the day's symbol set from an IBKR market scan (high-volatility
    // movers) then runs the strategy tournament on each pick, landing the
    // winners in the Trade tabs. Manual for now (Trade menu); stage 3 wires it
    // into the session schedule. Sequential and driven per frame like the
    // tournament/autopilot pumps.
    struct DailyLineup {
        enum class Phase { Idle, Scanning, FetchingBars, Ranking, Tournaments, Done };
        Phase phase = Phase::Idle;
        double stamp_s = 0;                  // phase-entry time, for timeouts
        net::ScanSpec spec;                  // the scan that seeds the pool
        tt::RankParams rank;                 // gates + top_n for the ranking
        std::vector<std::string> pool;       // scan-hit symbols (UI-thread copy)
        std::map<std::string, std::vector<tt::RankBar>> bars;  // fetched, per pool sym
        std::set<std::string> awaiting;      // pool syms whose bars aren't in yet
        std::vector<std::string> picks;      // ranked winners (installed as tabs)
        std::size_t tourn_idx = 0;           // next pick to run a tournament for
        bool autostart = false;              // on Done, start the live session
    };
    DailyLineup lineup_;
    // The scanner delivers hits on the I/O thread; hand them to the UI thread
    // under this lock (mirrors the pending-candle handoff pattern).
    std::mutex lineup_mu_;
    std::vector<net::ScanHit> lineup_hits_;
    bool lineup_hits_ready_ = false;
    // Symbols the FetchingBars phase is collecting, and the I/O thread's inbox
    // of arrived bars — both guarded by lineup_mu_ so on_candles never touches
    // the UI-thread DailyLineup directly.
    std::set<std::string> lineup_want_bars_;
    std::vector<std::pair<std::string, std::vector<tt::RankBar>>> lineup_bar_inbox_;
    void start_daily_lineup(bool autostart_when_done = false);  // Trade menu / schedule
    void pump_daily_lineup();                      // UI thread, per frame
    void collect_lineup_bars(net::CandleBatch& b); // on_candles tap during FetchingBars
    // Newest cached candles for a live symbol, oldest first, as engine bars.
    // Replayed through the strategy after on_init so lookback indicators are
    // warm — a live session's only other bar source is tick aggregation.
    std::vector<tt::Bar> seed_bars(const std::string& symbol, int bar_sec) const;
    // Symbols whose warmup history was not cached when the live session
    // started, mapped to their engine symbol id. The candle cache is
    // memory-only, so the first session after launch always finds it cold; we
    // fetch the bars and re-seed those symbols when they land. Touched from the
    // UI thread (session start) and the data thread (on_candles).
    std::mutex warmup_mu_;
    std::map<std::string, uint32_t> warmup_want_;
    bool lineup_active() const { return lineup_.phase != DailyLineup::Phase::Idle; }
    // True while any backtest/optimizer/tournament/lineup work is in flight —
    // gates the engine's strategy-log flood to the optimizer panel instead of
    // the live console. Must NOT rely on engine_.running() alone: a short
    // backtest flips running_ false before the UI drains its buffered flood, so
    // the tail (often the whole run) would leak into the live log. The outer
    // sweep/tournament/lineup flags stay set across all the sub-backtests.
    // One live-start path shared by the Trade panel's Start button and the
    // daily-lineup scheduler (extracted from the panel start callback so the
    // scheduler can't drift from the manual path).
    void start_live_session(const TradePanel::StartOpts& opts);
    // The active-account snapshot the Trade panel header shows; also feeds the
    // scheduler's auto-start so it routes to the same broker/sub-accounts.
    TradePanel::AccountInfo trade_account_info();
    // Fire start_daily_lineup on the configured pre-market clock (weekday,
    // once/day); on Done, auto-start unless lineup_propose_only.
    void pump_lineup_schedule();
    int lineup_last_build_day_ = -1;         // tm_yday guard: one scheduled build/day
    bool lineup_autostart_pending_ = false;  // Done -> start_live_session next frame
    char lineup_build_buf_[8] = "09:35";     // Trade-menu edit buffer for build time

    // Broker-disconnect watchdog: fire a webhook alert when the order path is
    // down for more than a minute during a live session. STATE-based (not a log
    // scan), so it catches even a silent freeze where nothing is logged — the
    // gap that let a gateway outage go unnoticed for hours. See pump_broker_watchdog.
    void pump_broker_watchdog();
    double broker_down_since_s_ = 0.0;       // GetTime when the broker first went down (0 = up)
    double broker_down_last_alert_s_ = 0.0;  // GetTime of the last down-alert (0 = none this episode)
    void pump_orphan_watchdog();
    double orphan_since_s_ = 0.0;            // GetTime an adopted position first showed up naked
    double orphan_last_alert_s_ = 0.0;       // GetTime of the last orphan alert (0 = none)

    // Daily-lineup live swap: when a scheduled (auto-start) build finishes while
    // a session is already running, cycle the session onto the new picks WITHOUT
    // flattening the symbols that carry over — those are re-adopted + held on the
    // restart (hold-until-flat). Only positions in symbols leaving the lineup are
    // closed. Multi-frame: cancel the dropped symbols' resting orders + market-
    // close their positions, wait until they're confirmed flat (or a deadline),
    // then safe_stop_live(keep) and restart on the new lineup.
    enum class SwapStage { None, Flatten, Restart };
    void begin_lineup_swap(const TradePanel::StartOpts& next);  // kick off the cycle
    void pump_lineup_swap();                 // UI thread, per frame: drive it
    SwapStage swap_stage_ = SwapStage::None;
    double swap_deadline_s_ = 0.0;           // ImGui::GetTime() give-up for the flatten
    std::vector<uint32_t> swap_flatten_ids_; // dropped symbol_ids being closed
    TradePanel::StartOpts swap_opts_;        // the new lineup to restart on

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

    // Loaded first (declaration order matters): the persisted trade route
    // decides which market-data backbone the members below are wired to.
    std::string config_path_;
    AppConfig cfg_;

    LogConsole log_;
    LogConsole opt_log_;   // optimizer/backtest output; kept out of the live log + /logs
    bool show_opt_log_ = false;
    // Send a log line to the optimizer panel (backtest/optimizer/tournament
    // output) or the main live console, by engine backtest-state + prefix.
    void route(std::string line);
    SeriesStore series_;
    QuoteBook quotes_;
    net::GatewayData gw_;
    net::TwsData tws_data_;
    // The active backbone. TWS route: everything (charts, watchlist, trading)
    // rides IB Gateway's socket and the CP web gateway is never started —
    // IBKR allows one brokerage session per username, so holding a CP web
    // session next to a TWS session makes the two kick each other forever.
    bool use_tws_data_ = false;
    net::IMarketData& data_;
    // Declared before engine_ on purpose: the engine's live thread holds a
    // raw pointer to the broker, so the broker must be destroyed after the
    // engine (members destruct in reverse declaration order).
    std::unique_ptr<IbkrBroker> ibkr_;
    std::unique_ptr<TwsBroker> tws_;   // same ordering contract as ibkr_
    // Rotates the TWS broker/feed API client ids across session starts so a quick
    // stop->start never reuses an id the just-reaped connection is still releasing
    // at the gateway (error 326: "client id already in use").
    int tws_client_seq_ = 0;
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
    std::unique_ptr<TwsFeed> tws_feed_;
    std::atomic<bool> rt_feed_active_{false};   // worker thread: skip snapshot ticks
    AlertNotifier alerts_;
    UpdateChecker update_;   // polls GitHub for a newer main than this build
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
    // Set at session start on a reconciling (TWS) route; cleared once the live
    // snapshot reports reconciled==true and we re-anchor the journal baseline
    // to the real account equity (see the journal block in the frame loop).
    bool journal_baseline_pending_ = false;

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

    // Last periodic config save (ImGui::GetTime()); saves run once a minute
    // so a force-killed process loses at most a minute of settings.
    double last_cfg_save_ = 0.0;

    // ---- diagnostics endpoint (net/diag_server.h) ----
    DiagServer diag_srv_;
    std::mutex diag_mu_;
    std::string diag_json_ = "{}";     // published by pump_diag(), read by the server
    std::string metrics_text_;         // Prometheus /metrics body, published alongside
    double diag_next_build_s_ = 0.0;   // next UI-thread re-render (ImGui::GetTime())
    std::time_t session_start_ = 0;    // wall-clock app start, for /diag uptime
    // Set by the diag server thread on POST /control/kill; consumed on the UI
    // thread (the sole producer to the engine's SPSC command ring).
    std::atomic<bool> diag_kill_requested_{false};

    bool should_quit_ = false;        // host loop exits when true
    bool pending_quit_ = false;       // quit awaiting the "live trading" confirm
    bool pending_signout_ = false;    // sign-out awaiting the "live trading" confirm
    bool pending_update_ = false;     // update awaiting the confirm dialog
    std::string update_dismissed_commit_;   // "Later"-ed commit: hide until main moves again
    bool update_check_click_ = false;   // Help > Check for updates clicked this frame
    bool update_check_wait_ = false;    // a manual check is in flight
    uint32_t update_check_gen_ = 0;     // check_count() snapshot when it started
    double update_check_started_ = 0.0; // GetTime() at start, for a timeout

    bool had_ini_ = false;
    bool layout_checked_ = false;
    // Frames drawn since launch, capped — used to restore Config::active_log on
    // frame 2, once the log windows' dock tabs exist.
    int log_focus_frames_ = 0;
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
