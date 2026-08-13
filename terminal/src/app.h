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
#include "lineup_dryrun.h"
#include "market_data.h"
#include "net/book_divergence.h"
#include "net/diag_server.h"
#include "net/gateway_data.h"
#include "net/hist_freshness.h"
#include "net/tws_data.h"
#include "symbol_params.h"   // LineupPlan, in the dry run's admission hook
#include "tick_clock.h"      // MonoClock — the clock every deadline below is on
#include "update_check.h"
#include "watchdog_timer.h"
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
#include <chrono>
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

    // ---- the two halves of a host-loop iteration --------------------------
    //
    // TICK the app, then (only if there is any point) DRAW it. The split is the
    // 2026-08-11 fix: every scheduler, state machine and watchdog lives in
    // tick(), which main.cpp calls unconditionally, so none of them can be
    // switched off by minimizing the window, docking a panel behind a sibling
    // tab, or disconnecting the RDP session that owns the display.
    //
    // Everything reachable from tick() is ImGui-FREE — there is no frame in
    // progress when it runs — and every deadline it evaluates is on mono_s()
    // rather than ImGui::GetTime(). Both halves are load-bearing; see
    // tick_clock.h for what happens if only one of them is done.
    //
    // Returns how many milliseconds the host may sleep before ticking again
    // (tick_sleep_ms): a period, so a tick that overran its slot returns 0. The
    // rendering path ignores it — the vsynced swap paces that loop already.
    int tick();

    // Draw one frame. PURELY presentation: reads state tick() advanced, submits
    // widgets, and decides nothing that has a clock on it. Skipping it must
    // never change what the app does, only what is on screen.
    void draw();

    // Window-close request from the host loop. Quits immediately if nothing is
    // trading; otherwise pops a confirm dialog and quits only once confirmed.
    void request_quit();
    bool should_quit() const { return should_quit_; }

    // Process exit status. Zero for every ordinary run; the headless dry run
    // (TT_AUTORUN_LINEUP=1) sets it so a scheduled job can branch on the build's
    // verdict without parsing the log. main() must return THIS, including from
    // its shutdown watchdog — a watchdog that force-exits 0 would turn every
    // slow teardown into a green run.
    int exit_code() const { return exit_code_; }

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
    // Wait (bounded) for reap_async'd broker adapters to finish being destroyed,
    // so a new session never dials the fixed orders client id while this process
    // still holds it. See the definition.
    void wait_for_broker_reap(const char* who);
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
        // The id request_candles handed back. A delivery is OURS only if it
        // carries this id — see SweepSetup::req_id for the 2026-08-10 incident
        // that matching on symbol + interval caused.
        uint32_t req_id = 0;
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
    // True iff a candle fetch is now outstanding for this sweep. False means
    // nothing was armed and sweep_setup_ was left untouched — which the caller
    // MUST NOT read as "the fetch settled with nothing" (see queue_sweep).
    bool queue_sweep(const SweepPanel::Request& rq, bool for_tournament = false);
    void stash_pending_sweep(net::CandleBatch& batch);   // IPC thread
    // Drop a candle request the caller has given up waiting for, so a late batch
    // cannot start a sweep whose owner is gone (see SweepSetup::for_tournament).
    void cancel_pending_sweep();
    // Is a queued sweep still waiting on (or holding) its bars? False means the
    // fetch is settled one way or the other, so pump_tournament can stop
    // burning its fetch budget on a request the feed has already errored out.
    bool sweep_fetch_pending() const;
    void pump_sweep();                                   // UI thread, per frame
    // One finished backtest, in the sweep state machine. Called in a loop by
    // pump_sweep under a frame budget — the one-result-per-frame version made
    // every cell cost a whole vsync period (see kSweepDrainBudgetMs).
    void consume_sweep_result(BacktestResult& r);
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
        // The daily lineup started this one, so its outcome is the lineup's to
        // record. Asking "is lineup_.phase == Tournaments?" instead was wrong:
        // pump_autopilot runs BEFORE pump_daily_lineup in the frame and has no
        // lineup guard, so on the frame a lineup tournament ends the autopilot
        // can grab the free optimizer and its failure would be filed against the
        // lineup's pick. Who STARTED it is the only thing that answers this.
        bool for_lineup = false;
        double stamp_s = 0;          // phase-entry time, for timeouts
        double start_s = 0;          // tournament-entry time, for the hard deadline
        // Candidates already given a second turn at the back of the field, so a
        // fetch that keeps timing out cannot be requeued forever. See
        // kTournFetchTimeoutS.
        std::set<std::string> requeued;
        // "waiting for the data session" is logged once per wait, not once per
        // frame: Phase::Launch runs every frame and a reconnect takes seconds.
        bool feed_wait_logged = false;
    };
    Tournament tourn_;
    // candidates: explicit strategy keys to race; empty = built-in + all loaded.
    void start_tournament(SweepPanel::Request rq, const std::string& target_symbol,
                          std::vector<std::string> candidates = {},
                          bool for_lineup = false);
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
        // Picks whose tournament crowned a champion and INSTALLED it on their
        // own tab. Recorded positively, by finish_tournament at the install
        // line: a pick missing from this set has no parameters fitted to itself,
        // so Phase::Done refuses to let it trade another symbol's set (see
        // SymbolOutcome::fitted_this_build for why the failed-list inverse was
        // wrong — most failure paths never reach finish_tournament at all).
        std::set<std::string> fitted;
        std::size_t tourn_idx = 0;           // next pick to run a tournament for
        bool autostart = false;              // on Done, start the live session
        // When FetchingBars was ENTERED, as opposed to stamp_s which every
        // arrival resets. The quiet timer alone could not tell "these requests
        // failed" from "these requests are being paced": a send gate holding the
        // ranking pass produces no arrivals at all, so the quiet timer expired
        // with the pool unfetched and the day's symbols got picked from whichever
        // handful had already been sent. See kLineupFetchDeadlineS.
        double fetch_start_s = 0;
    };
    DailyLineup lineup_;
    // The scanner delivers hits on the I/O thread; hand them to the UI thread
    // under this lock (mirrors the pending-candle handoff pattern).
    std::mutex lineup_mu_;
    std::vector<net::ScanHit> lineup_hits_;
    bool lineup_hits_ready_ = false;
    // Requests the FetchingBars phase is collecting (request id -> pool symbol),
    // and the I/O thread's inbox of arrived bars — both guarded by lineup_mu_ so
    // on_candles never touches the UI-thread DailyLineup directly.
    //
    // Keyed by REQUEST ID rather than by symbol, for the reason SweepSetup::req_id
    // gives: a chart or warmup fetch of the same symbol's daily bars over a
    // different span would otherwise be ranked as if it were the ranking pass's
    // own 1mo fetch, and realized volatility computed off the wrong window picks
    // the wrong symbols for the whole day.
    std::map<uint32_t, std::string> lineup_want_bars_;
    std::vector<std::pair<std::string, std::vector<tt::RankBar>>> lineup_bar_inbox_;
    // Pool symbols whose ranking fetch was ERRORED out by the feed (I/O thread,
    // on_error). Without this the FetchingBars phase could only learn that a
    // request had failed by waiting for a quiet timer — which is also what fires
    // when requests are merely being PACED, and the two are indistinguishable
    // from the UI thread. See pump_daily_lineup.
    std::vector<std::string> lineup_dropped_;
    void start_daily_lineup(bool autostart_when_done = false);  // Trade menu / schedule
    void pump_daily_lineup();                      // UI thread, per frame
    void collect_lineup_bars(net::CandleBatch& b); // on_candles tap during FetchingBars
    // Newest cached candles for a live symbol, oldest first, as engine bars.
    // Replayed through the strategy after on_init so lookback indicators are
    // warm — a live session's only other bar source is tick aggregation.
    std::vector<tt::Bar> seed_bars(const std::string& symbol, int bar_sec) const;
    // Warmup fetches outstanding for symbols whose history was not cached when
    // the live session started: REQUEST ID -> the symbol and its engine symbol
    // id. The candle cache is memory-only, so the first session after launch
    // always finds it cold; we fetch the bars and re-seed those symbols when they
    // land. Touched from the UI thread (session start) and the data thread
    // (on_candles, on_error).
    //
    // Keyed by request id, like lineup_want_bars_ and SweepSetup::req_id, and for
    // the same reason. Keyed by SYMBOL, ANY delivery of that symbol consumed the
    // ticket — a chart re-request at a different interval, which a data-session
    // reconnect triggers on its own (ChartPanel::draw re-fetches on a
    // connection_generation bump). seed_bars only reads the WARMUP interval, so
    // the stolen ticket reseeded nothing and the real warmup batch, arriving
    // afterwards, found no entry and was ignored. The strategy then ran the whole
    // session with zero warmup bars against a several-hundred-bar lookback: the
    // documented root cause of a day with no live trades.
    struct WarmupWant {
        std::string symbol;
        uint32_t sid = 0;   // engine symbol id, for reseed_symbol
    };
    std::mutex warmup_mu_;
    std::map<uint32_t, WarmupWant> warmup_want_;
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
    // The strategy's declared parameter specs, in the shape the Trade panel
    // wants. One definition instead of the identical lambda every call site was
    // spelling out — admission now needs it too, and it must not drift.
    TradePanel::ParamSpecsFn param_specs_fn();
    // Lineup verdicts the operator has to be able to read REMOTELY. route()
    // files anything prefixed "lineup:" into optimizer.log, which /logs, /events
    // and the alert channel never see — the same trap that hid the live output
    // during the zero-live-trades investigation. Before 0.16.0 the lineup always
    // started something, so "no session today" was not even a reachable outcome;
    // now it is, and it must not be reachable silently.
    void route_operator(std::string line);
    // symbol -> "own" / "mixed" / "inherited" / "none": where the RUNNING
    // session's parameters for that symbol came from, latched at start and
    // reported by /diag. On 2026-08-10 all six symbols inherited one SSPC fit
    // and no field anywhere said so (see symbol_params.h).
    std::map<std::string, std::string> live_param_source_;
    // The active-account snapshot the Trade panel header shows; also feeds the
    // scheduler's auto-start so it routes to the same broker/sub-accounts.
    TradePanel::AccountInfo trade_account_info();
    // Fire start_daily_lineup on the configured pre-market clock (weekday,
    // once/day); on Done, auto-start unless lineup_propose_only.
    void pump_lineup_schedule();
    int lineup_last_build_day_ = -1;         // tm_yday guard: one scheduled build/day
    bool lineup_autostart_pending_ = false;  // Done -> start_live_session next frame
    char lineup_build_buf_[8] = "09:35";     // Trade-menu edit buffer for build time

    // ---- TT_AUTORUN_LINEUP=1: headless daily-lineup dry run ----------------
    // Drives ONE full build — scan, volatility rank, per-symbol tournaments —
    // through start_daily_lineup(), the same call the 09:35 schedule makes, then
    // reports it in machine-readable "dryrun:" lines (see lineup_dryrun.h) and
    // exits with the build's verdict. It is a pure OBSERVER of the lineup state
    // machine: it triggers the build and measures it, and every decision it
    // reports was made by the production code. A dry run that ran its own copy
    // of the logic would be evidence about the copy.
    //
    // PROPOSE-ONLY IS FORCED, not configured. start_daily_lineup is called with
    // autostart_when_done=false, pump_lineup_schedule() is off for the run (it
    // honours cfg_.lineup_propose_only and would auto-start), and
    // start_live_session() — the one choke point every start path goes through —
    // refuses outright. config.json cannot make this run trade.
    struct LineupDryRun {
        bool active = false;      // TT_AUTORUN_LINEUP was set at construction
        bool triggered = false;   // the build has been kicked off (once per process)
        bool running = false;     // a build is in flight and being measured
        int64_t start_ms = 0;     // mono ms at the trigger, for total_ms
        int64_t phase_ms = 0;     // mono ms the current lineup phase was entered
        DailyLineup::Phase phase = DailyLineup::Phase::Idle;   // last phase observed
        std::vector<DryRunSymbol> syms;   // one per pick, in tournament order
        std::string tourn_symbol;         // pick whose tournament is in flight ("" = none)
        int64_t tourn_start_ms = 0;
        // Admission ran and refused the whole day (LineupPlan::start false). It
        // reaches Phase::Done like a good build does, so "which phase did it die
        // in" cannot see it — and a build that runs every tournament and then
        // starts nothing is the single most important thing a dry run can find.
        bool admission_refused = false;
        // Fetch counters at the trigger. The source's counters are
        // process-lifetime (a chart or warmup fetch spends them too), so the
        // summary reports the DELTA across the build rather than the totals.
        net::HistStats hist0;
        DryRunSummary sum;
    };
    LineupDryRun dry_;
    static const char* dryrun_phase_name(DailyLineup::Phase p);
    void pump_lineup_dryrun();          // UI thread, per frame: trigger, then observe
    void dryrun_tournament_settled();   // pump_daily_lineup: a pick's tournament ended
    void dryrun_record_plan(const LineupPlan& plan);   // pump_daily_lineup: admission
    void dryrun_finish(const std::string& abort);      // summary + exit code + quit

    // Broker-disconnect watchdog: fire a webhook alert when the order path is
    // down for more than a minute during a live session. STATE-based (not a log
    // scan), so it catches even a silent freeze where nothing is logged — the
    // gap that let a gateway outage go unnoticed for hours. See pump_broker_watchdog.
    void pump_broker_watchdog();
    WatchdogTimer broker_wd_;                // grace + re-alert bookkeeping (mono_s)
    void pump_orphan_watchdog();
    WatchdogTimer orphan_wd_;                // ditto, for adopted-but-naked positions

    // Book-divergence auditor: re-reads the ACCOUNT's positions every minute
    // during a live session and compares them against the engine's book. It is
    // the layer that has to work whether or not the client-id fix is perfect —
    // on 2026-08-13 a phantom 411-share position was published as real, with a
    // fictional +$528 unrealized, for 4 h 03 m, and no invariant anywhere
    // noticed. Detect-only: it pages and populates /diag, and never seeds,
    // halts, cancels or flattens. See net/book_divergence.h.
    void pump_book_audit();
    net::BookAudit book_audit_;
    WatchdogTimer book_div_wd_;              // page/re-page cadence for a confirmed divergence
    WatchdogTimer book_blind_wd_;            // ditto for "the auditor is not being answered"
    double book_audit_next_s_ = 0.0;         // mono_s() of the next request
    // The app-side quantities as they were when the in-flight audit was
    // REQUESTED. Half of the race handling: any symbol whose quantity differs by
    // the time the answer lands moved mid-audit (an ordinary fill), and
    // comparing two books taken at different instants is not evidence of
    // anything — those symbols are dropped from the round. The other half is
    // net::kBookAuditConfirmRounds, which covers a fill the broker has already
    // executed but the engine has not yet drained.
    std::vector<net::BookPos> book_audit_sent_;
    // What the operator is still owed an answer about. The page, the all-clear
    // and tt_book_divergences all read this rather than BookAudit::confirmed(),
    // because confirmed() empties the moment the state leaves Diverged —
    // including into Suspect (the quantities merely moved) and Blind (nothing is
    // answering any more), neither of which is a recovery. See DivergenceLatch.
    net::DivergenceLatch book_div_;

    // History-staleness watchdog: pages when a traded symbol's bars stop being
    // refreshed while the data socket still reports connected. The 2026-08-07
    // outage was invisible to every existing signal (see net/hist_freshness.h);
    // this measures the SUCCESSES instead of the in-flight requests. Diagnostic
    // only — it never halts, cancels or flattens.
    void pump_history_watchdog();
    net::HistoryFreshness hist_fresh_;       // fed from the on_candles success path
    // The "since" of the since/last-alert pair is the SESSION clock, not the
    // start of the stale episode: staleness is already measured in absolute bar
    // age, and a symbol that has never been answered at all has to be aged from
    // something — session start is the only honest anchor (and makes the grace
    // period double as a settle-in window after a restart).
    //
    // Not a WatchdogTimer: `since` here is the SESSION clock rather than the
    // start of a stale episode, and the clear path deliberately leaves it alone.
    double hist_live_since_s_ = 0.0;         // mono_s the current live session was first seen
    double hist_stale_last_alert_s_ = 0.0;   // mono_s of the last stale-bars alert (0 = none)
    // The interval the autopilot re-fetches, i.e. the series that is supposed
    // to stay fresh. Shared by /diag, /metrics and the watchdog so all three
    // report the same thing.
    std::string traded_bar_interval() const;

    // Pre-open gateway AUTHENTICATION check: 08:45-09:15 local on a trading day,
    // once, level-triggered inside the window with a settle time. The ONLY check
    // that runs without a live session — every other one is gated on
    // live_running() and was therefore blind while a failed overnight re-login
    // burned 13 hours on 2026-08-09. Pages if the gateway is up but not logged
    // in, then makes AT MOST ONE relaunch attempt (day stamp + paper only + the
    // script's own governor: IBKR locks accounts on repeated failed logins) and
    // polls the outcome. Runs from tick(), so no frame is needed.
    void pump_preopen_gateway_check();
    bool preopen_authed() const;             // farms up, or a live order path reaching IBKR
    int64_t preopen_day_ = -1;               // tm_year*400+tm_yday of the last check
    int64_t preopen_relaunch_day_ = -1;      // ...of the last relaunch ATTEMPT (never twice)
    double preopen_bad_since_ = 0.0;         // mono_s when it first read not-authed (0 = it doesn't)
    double preopen_verdict_until_ = 0.0;     // mono_s deadline for the post-relaunch verdict (0 = none)

    // Steady seconds/ms since construction — THE clock for every deadline in
    // this class, because tick() runs on iterations where no frame is built and
    // ImGui::GetTime() stands still there (and then jumps by the whole gap on
    // the next frame). See tick_clock.h for the incident this replaced.
    double mono_s() const { return mono_.now_s(); }
    int64_t mono_ms() const { return mono_.now_ms(); }
    MonoClock mono_;

    // Daily-lineup live swap: when a scheduled (auto-start) build finishes while
    // a session is already running, cycle the session onto the new picks WITHOUT
    // flattening the symbols that carry over — those are re-adopted + held on the
    // restart (hold-until-flat). Only positions in symbols leaving the lineup are
    // closed. Multi-frame: cancel the dropped symbols' resting orders + market-
    // close their positions, wait until they're confirmed flat (or a deadline),
    // then safe_stop_live(keep) and restart on the new lineup.
    enum class SwapStage { None, Flatten, Restart };
    void begin_lineup_swap(const TradePanel::StartOpts& next);  // kick off the cycle
    void pump_lineup_swap();                 // UI thread, per tick: drive it
    SwapStage swap_stage_ = SwapStage::None;
    double swap_deadline_s_ = 0.0;           // mono_s() give-up for the flatten
    std::vector<uint32_t> swap_flatten_ids_; // dropped symbol_ids being closed
    TradePanel::StartOpts swap_opts_;        // the new lineup to restart on
    // How long the Restart stage will wait for the OLD broker's destructor to
    // return before starting the new session on the same (fixed) client id.
    // Five seconds is several times the ~1 s the old I/O thread can spend parked
    // in waitForSignal before it notices stop_ and disconnects, and it is a
    // deadline rather than a sleep: the ordinary case clears in milliseconds and
    // the swap proceeds on that frame. See pump_lineup_swap.
    static constexpr double kSwapReapWaitS = 5.0;
    double swap_reap_deadline_s_ = 0.0;      // mono_s() give-up for that wait

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
    // Rotates the TWS FEED API client id across session starts so a quick
    // stop->start never reuses an id the just-reaped connection is still
    // releasing at the gateway (error 326: "client id already in use").
    //
    // The BROKER used to share this counter and no longer does: its id is fixed
    // at kTwsOrdersClientId. IB scopes an order's fills and cancels to the
    // client that placed it, so a session that rotates onto a new id adopts
    // resting orders it cannot hear — the 2026-08-13 phantom position. The feed
    // has no such tie: it places nothing, so nothing is scoped to it.
    int tws_feed_seq_ = 0;
    // Broker teardowns handed to detached threads and not yet finished. The
    // lineup swap waits on this before starting a new session, because with a
    // FIXED orders client id a restart 16 ms after the reap would collide with
    // our own socket — reap_async detaches, and the old I/O thread can sit in
    // waitForSignal for up to a second before it notices stop_ and calls
    // eDisconnect. This is the only bounded wait that is safe here: it waits on
    // a fact this process owns and can observe, not on a guess about how long
    // the gateway holds a released id.
    std::shared_ptr<std::atomic<int>> broker_reaps_ =
        std::make_shared<std::atomic<int>>(0);
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
        // The id request_candles handed back for THIS sweep's bars, and the only
        // thing a delivery is matched on.
        //
        // Matching on symbol + interval instead is how, on 2026-08-10, a sweep
        // labelled "SOXL 5m 6mo" ran on 1567 bars — a live-warmup fetch of the
        // same symbol and the same interval over a completely different span (a
        // real 6mo request returns ~2966-9517 bars). It optimized on the wrong
        // dataset, silently, and finished 3 seconds after its tournament had
        // already declared "no candidate produced a result", so its +0.1213
        // holdout Sharpe — the only positive score in the entire build — was
        // thrown away. Nothing in the log said the data was wrong, because as far
        // as every label was concerned it was not.
        //
        // 0 = no request outstanding, which never matches a delivery: request ids
        // start at 1 (TwsData::next_id_).
        uint32_t req_id = 0;
        SweepPanel::Request req;
        std::vector<Bar> bars;
        IStrategy* strategy = nullptr;   // leased instance
        std::string key;                 // strategy key, for the label
        std::map<std::string, double> params;   // captured at queue time
        // Who asked for this sweep, decided when it was QUEUED. Reading
        // tourn_.active at completion instead was the 0.16.0 blocker: a
        // candidate abandoned at the 60s Queued timeout leaves its candle
        // request outstanding, and stash_pending_sweep matches on symbol +
        // interval only, so a late batch could still fire that sweep long after
        // the tournament ended — and it then completed as a "manual run",
        // writing an unadjudicated in-sample winner into the shared map. That is
        // exactly defect A, back again by the very timeouts that caused it.
        bool for_tournament = false;
    };
    SweepSetup sweep_setup_;
    SweepPanel::State sweep_;
    BacktestConfig sweep_base_;          // train slice
    std::vector<Bar> sweep_test_bars_;   // holdout slice (never optimized on)
    bool sweep_holdout_phase_ = false;
    // SweepSetup::for_tournament, latched onto the RUNNING sweep when its bars
    // are consumed (sweep_setup_ is reusable the moment that happens).
    bool sweep_for_tournament_ = false;
    // A manual sweep's winner, waiting on its holdout run before it is written
    // to the swept symbol's tab. Only the names the sweep actually TUNED: the
    // rest of opt_.best is just the strategy-wide starting point, and writing
    // those onto the symbol would silently overwrite ten per-symbol values the
    // sweep is forbidden to touch (sweep_param_is_fixed). Held back until the
    // holdout scores because AdmitOwnPrevious will trade this set unattended
    // tomorrow morning — an in-sample winner that then fails its holdout must
    // not become the symbol's "own" fit.
    std::string sweep_fit_symbol_;
    std::map<std::string, double> sweep_fit_params_;
    IStrategy* sweep_strategy_ = nullptr;

    mutable std::mutex pending_bt_mu_;   // mutable: sweep_fetch_pending() is const
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
    // Holds a mono_s() deadline; 0 = not starting.
    double gateway_starting_until_ = 0.0;

    // Last periodic config save (mono_s()); saves run once a minute so a
    // force-killed process loses at most a minute of settings.
    double last_cfg_save_ = 0.0;

    // ---- diagnostics endpoint (net/diag_server.h) ----
    DiagServer diag_srv_;
    std::mutex diag_mu_;
    std::string diag_json_ = "{}";     // published by pump_diag(), read by the server
    std::string metrics_text_;         // Prometheus /metrics body, published alongside
    double diag_next_build_s_ = 0.0;   // next UI-thread re-render (mono_s())
    std::time_t session_start_ = 0;    // wall-clock app start, for /diag uptime
    // Set by the diag server thread on POST /control/kill; consumed on the UI
    // thread (the sole producer to the engine's SPSC command ring).
    std::atomic<bool> diag_kill_requested_{false};

    int exit_code_ = 0;               // process status; see exit_code()
    bool should_quit_ = false;        // host loop exits when true
    bool pending_quit_ = false;       // quit awaiting the "live trading" confirm
    bool pending_signout_ = false;    // sign-out awaiting the "live trading" confirm
    bool pending_update_ = false;     // update awaiting the confirm dialog
    std::string update_dismissed_commit_;   // "Later"-ed commit: hide until main moves again
    bool update_check_click_ = false;   // Help > Check for updates clicked this frame
    bool update_check_wait_ = false;    // a manual check is in flight
    uint32_t update_check_gen_ = 0;     // check_count() snapshot when it started
    double update_check_started_ = 0.0; // mono_s() at start, for a timeout

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
