#pragma once
// Backtest engine: a feed thread replays bars (plus synthetic intrabar
// O/H/L/C ticks) into md_ring at full speed; the engine thread consumes,
// advances the BacktestClock, runs the strategy, simulates execution, and
// tracks the portfolio. One consumer thread + a deterministic clock + seeded
// RNG => bit-identical reruns. The live paper path (Phase 4) reuses this
// loop with a RealTimeClock — the clock is the only difference.

#include "engine/clock.h"
#include "engine/events.h"
#include "engine/exec_sim.h"
#include "engine/latency.h"
#include "engine/portfolio.h"
#include "engine/reject.h"
#include "engine/spsc_ring.h"
#include "engine/tick_log.h"
#include "tt/strategy_api.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt {

struct BacktestConfig {
    std::string symbol;
    std::vector<Bar> bars;                  // ts_ns ascending (bar open times)
    double initial_cash = 100'000.0;
    ExecParams exec{};
    std::map<std::string, double> params;   // strategy parameters
    bool synth_ticks = true;                // O/H/L/C intrabar ticks for limit fills
    // THE DAY BOUNDARY THE LIVE ENGINE HAS AND THE REPLAY DID NOT. Local hour at
    // which a bar crossing it cancels every resting order and market-closes the
    // position, mirroring run_live's 15:57 EOD backstop (kEodBackstopH). 0 = off.
    //
    // Why it is a config knob and defaults OFF: a manual backtest and a replay
    // must keep judging the data's own clock — a daily-bar series has no
    // intraday boundary to honour, and a captured replay is meant to reproduce
    // what happened, not to improve on it. It is turned ON for the OPTIMIZER
    // path only (App::pump_sweep's sweep_base_), where the whole purpose is to
    // score a fit that will be TRADED.
    //
    // 2026-08-14 is why it exists. Engine::run replayed six months straight
    // through with no session concept, so the tournament scored an STKH
    // bollinger_reversion fit with time_stop = 233 bars at +8.60% — a 19.4-hour
    // hold on a 300 s series. Live, everything is force-flattened at 15:57, so
    // 77 bars is the ceiling and that time stop could never fire. It was the
    // strategy's ONLY exit for a losing position (no price stop by design), and
    // the position lost $506 in the 2 h 16 m before the backstop liquidated it.
    // The optimizer was fitting under physics production does not have.
    //
    // Clamping the parameter afterwards (tt::ui::time_stop_reachable) is a guard
    // rail; this is the source fix. Without it the optimizer keeps proposing
    // unreachable holds for every time-based parameter of every strategy, and
    // each one has to be caught by hand.
    double eod_flatten_h = 0.0;
};

struct TradeRow {
    int64_t ts_ns;
    uint8_t side;      // tt::Side
    double qty, price, fee;
};

struct BacktestResult {
    std::string symbol;
    double initial_cash = 0, final_equity = 0, total_return = 0;
    double max_drawdown = 0, sharpe = 0, win_rate = 0;
    int trades = 0, wins = 0, losses = 0;
    std::vector<double> eq_ts;      // epoch seconds (ImPlot time axis)
    std::vector<double> eq_val;
    std::vector<TradeRow> fills;
    int64_t lat_p50 = 0, lat_p99 = 0, lat_max = 0;
    uint64_t lat_count = 0;
    double duration_ms = 0;
    uint64_t events = 0;
};

// ---------------------------------------------------------------- live mode

struct RiskLimits {
    double max_order_qty = 1'000;
    double max_position_qty = 5'000;
    // Dollar cap on a position's size (|qty|*price). A position-INCREASING order
    // that would breach it is DOWN-SIZED to fit (not rejected), so one trade can
    // never risk more than a slice of the daily-loss budget on an adverse move —
    // the share caps above are notional-blind and miss this. Reduce/close orders
    // are never touched. 0 = disabled. Usually derived from daily_max_loss.
    double max_position_notional = 0;
    double price_band_pct = 0.20;   // limit orders within ±20% of last trade

    // Automated halts — the engine pulls the kill switch itself (cancel all,
    // flatten, halt strategy). 0 = disabled.
    double daily_max_loss = 0;      // $ of equity lost since session start
    double max_drawdown_pct = 0;    // fraction lost from the session equity high
    int stale_feed_sec = 0;         // no ticks this long with a position open
    // App-honored preference (the engine ignores it): when set, this symbol does
    // NOT arm the session's equity halts (daily-loss / drawdown) — positions are
    // held instead of force-flattened at the limit, to be closed by the strategy
    // or when they recover. The notional cap and stale-feed halt still apply. App
    // implements it by dropping the symbol from the halt aggregation.
    bool disable_auto_halt = false;
};

class IBrokerAdapter;

struct LiveConfig {
    std::vector<std::string> symbols;
    double initial_cash = 100'000.0;
    ExecParams exec{};
    std::map<std::string, double> params;
    // Optional per-symbol strategy params (parallel to symbols). ctx.param()
    // resolves against the current symbol's map; empty = use `params`.
    std::vector<std::map<std::string, double>> symbol_params;
    RiskLimits risk{};              // session-level: equity + stale-feed halts
    // Optional per-symbol order-level risk (parallel to symbols; empty = use
    // `risk`). Applied per order in EngineCtx; the equity/stale halts above stay
    // session-wide (per-symbol halting needs per-symbol portfolios).
    std::vector<RiskLimits> symbol_risk;
    // HOLD-ONLY symbols (parallel to symbols; empty = none, nonzero = hold-only).
    //
    // A symbol carried in the session ONLY so an existing broker position can be
    // adopted, audited and closed - never to open a new one. A lineup swap uses
    // this for a symbol it is dropping while the broker still shows a position
    // there: a symbol outside cfg.symbols can be neither adopted, audited nor
    // flattened (2026-08-06 orphan $846; 2026-07-21 NVDA, undetected 24 days),
    // so it has to stay - but it must not trade.
    //
    // It is a FIRST-CLASS FLAG rather than "leave the strategy key empty",
    // because an empty key is not the absence of a strategy: App::acquire_strategy
    // maps "" to kBuiltinStrategyKey ("sma_crossover.cpp"), a real promoted
    // strategy, which then runs on its declared defaults and opens a position the
    // moment adopt_hold releases the symbol.
    std::vector<uint8_t> symbol_hold_only;
    // THE ENTRY GATE (0.23.0). Answers, for a wall-clock instant: may a strategy
    // OPEN or ADD to a position right now? Empty = disarmed, which is what
    // backtests and replays want (their clock is the data's, not the world's).
    //
    // Why it exists, and why WALL CLOCK. On 2026-08-13 two strategies signalled
    // entries at 16:00:12 and 16:00:17 ET and IBKR refused both with error 201
    // "Exchange is closed"; a third at 16:05 was ACCEPTED and rested overnight as
    // a market order for the next open. Two independent holes produced that:
    //
    //   1. rsi2_pullback gates on hour_of_day_local(bar.ts_ns), and ts_ns is the
    //      bar's OPEN time while the order is placed one bar-length later. Its
    //      enter_until_h of 16 therefore really means 16:05 on 5m bars, so the
    //      last bar of every trading day escapes the window.
    //   2. bollinger_reversion, donchian_trend and scalper_burst ship
    //      enter_from_h 0 / enter_until_h 24 - no window at all. KORU entered at
    //      04:05 on 2026-08-13 under that; IBKR held the order and filled it at
    //      the 09:30 open, 5.4 hours after the signal that justified it.
    //
    // Fixing the strategies one at a time fixes neither the next strategy nor
    // the optimizer, which is free to fit the window to anything. So the gate
    // lives here, below every strategy, and is asked about NOW rather than about
    // the bar - a clock the bar-open bug cannot reach.
    //
    // It gates position-INCREASING strategy orders only. Exits, reduces, manual
    // orders, flattens and the kill switch are never gated: refusing to close is
    // how a position ends up naked, which is a far worse failure than a late
    // entry. See EngineCtx::submit_order.
    std::function<bool(int64_t now_ns)> entry_gate;
    // Optional per-symbol seed bars (parallel to symbols), replayed through the
    // strategy right after on_init so indicators are warm when the session
    // starts. Live bars only ever come from tick aggregation, so a strategy
    // whose lookback exceeds one session's worth of bars could otherwise NEVER
    // warm up — and the daily lineup swap re-inits every morning. Orders are
    // suppressed during the replay. Oldest first, same bar size as the symbol.
    std::vector<std::vector<Bar>> symbol_warmup;
    int bar_seconds = 60;           // tick->bar aggregation for on_bar (fallback)
    // Optional per-symbol bar size (parallel to symbols; 0/absent = bar_seconds).
    std::vector<int> symbol_bar_seconds;
    // Optional real-broker routing (caller-owned, must outlive the session).
    // Null = orders fill in ExecSim as before.
    IBrokerAdapter* broker = nullptr;
    // Non-empty: append every consumed market-data event to this .ttk file
    // for deterministic replay later.
    std::string capture_path;
    // True: never sleep while idle — the engine thread spins (_mm_pause) and
    // handles the next tick within nanoseconds instead of up to 5 ms.
    // Dedicates most of a core; set it when a real-time feed drives the
    // session. False keeps the sleep tiers sized for delayed quotes.
    bool busy_spin = false;
    // >= 0: pin the engine thread to this core (avoids scheduler migrations
    // and the cache refills they cost). Pick a core the feed isn't on.
    int pin_core = -1;
    // Per-symbol strategy watchdog: if a strategy's on_tick/on_bar callback runs
    // longer than this many ms, that symbol is halted + flattened so one slow or
    // runaway strategy can't keep degrading the shared engine thread. 0 = off.
    int watchdog_ms = 0;
};

// Replay a captured .ttk session through ExecSim with the deterministic
// backtest clock: same ticks in, bit-identical run out.
struct ReplayConfig {
    std::string name;               // shown as the result's symbol column
    TickLog log;
    double initial_cash = 100'000.0;
    ExecParams exec{};
    std::map<std::string, double> params;
    // >0: re-aggregate the recorded ticks into bars of this size instead of the
    // size baked into the recording (log.bar_seconds). Lets one .ttk be replayed
    // at several bar sizes to compare. 0 = use the recorded size.
    int bar_seconds_override = 0;
};

enum class OrderStatus : uint8_t { Working = 0, Filled, Cancelled, Rejected };

struct OrderRecord {
    uint64_t id = 0;
    int64_t ts_ns = 0;
    uint32_t symbol_id = 0;
    std::string symbol;
    uint8_t side = 0, type = 0;
    OrderStatus status = OrderStatus::Working;
    double qty = 0, limit_price = 0, fill_price = 0, fee = 0;
    bool manual = false;
    // WHY, when status == Rejected. The invariant, enforced at every write site
    // and asserted by engine/tests/test_reject.cpp:
    //
    //     status == Rejected  =>  reject_cause != None && !reject_msg.empty()
    //
    // reject_code is the BROKER's number when it gave one (IBKR 201 "Exchange is
    // closed"); it stays 0 for every locally-refused order, and 0 no longer means
    // "no reason" - reject_cause always does say. Before 0.23.0 a rejected record
    // could carry cause-less 0/"" and did, twice, on 2026-08-13.
    RejectCause reject_cause = RejectCause::None;
    int reject_code = 0;
    std::string reject_msg;
};

// One row per (symbol, cause) refusal pair for the whole session: the complete
// count, even for the causes that are too chatty to give a blotter row each.
//
// The blotter (`orders`) records only the FIRST refusal of a pair, because it is
// a 200-entry ring: a strategy retrying an entry into a closed session every
// minute would evict every real order in the session and the truth would be gone
// - the same "instrumentation drowns itself" shape as the history watchdog's
// repeat pages. This table never floods: it is bounded by symbols x causes, and
// `count` + `last_ts_ns` say how bad and how recent.
//
// HEALTHY vs BROKEN, explicitly: an ordinary live session has ONE row per
// symbol, cause warmup_replay, count = the number of seed bars that produced a
// signal, last_ts_ns pinned to the warmup that ended at session start. Anything
// else - any row whose cause is not warmup_replay, or a warmup_replay row whose
// last_ts_ns keeps advancing after the session is warm - is a defect.
struct RefusalStat {
    uint32_t symbol_id = 0;
    std::string symbol;
    RejectCause cause = RejectCause::None;
    int code = 0;              // broker's number, 0 for local refusals
    std::string message;
    uint64_t count = 0;
    int64_t first_ts_ns = 0, last_ts_ns = 0;
    // The refused order was REDUCING or CLOSING a position. This is the field
    // that decides whether a refusal pages Critical: an exit that cannot be
    // placed is how a position ends up with nothing that will ever close it.
    // STICKY: true if ANY refusal of this pair was an exit, which is why it may
    // not be multiplied by `count` — see exit_count.
    bool exit_order = false;
    // How many of `count` were exits. /diag's refused_exits and the
    // tt_refused_exits metric sum THIS, not count: both are documented as
    // "there is no benign value but 0", and summing count made them report
    // every entry refusal that happened to share a (symbol, cause) row with one
    // exit — 189 for a single held position in the 0.23.0 review.
    uint64_t exit_count = 0;

    // ---- alerting bookkeeping (run_live only; not published) ----------------
    // Kept on the row because the row IS the per-(symbol, cause) state, and an
    // alert decision that lives anywhere else drifts from the counter it reads.
    uint64_t exit_pages = 0;        // Critical pages emitted for this pair
    int64_t exit_paged_ns = 0;      // when the last one went out
    int64_t exit_repage_ns = kExitRefusalRepageNs;   // wait before the next
    uint64_t warn_at = kRefusalRepeatAlertAt;        // count that re-arms the Warning
};

// One entry per symbol in a live session, in symbol_id order (index 0 = id 1).
struct SymbolState {
    std::string symbol;
    double last_price = 0.0;
    Position position{};
    // Commissions paid on this symbol so far. Position::realized_pnl is GROSS of
    // them (Portfolio::apply charges the fee to cash only), so realized minus
    // this is what the account actually kept. On 2026-08-14 the difference was
    // $15.08 on a reported -$597.01, and the reported figure is the one used to
    // judge whether a strategy has an edge. See Portfolio::fees().
    double fees = 0.0;
    // The parameters this symbol's strategy is ACTUALLY running, as the engine
    // holds them. The terminal keeps its own copy in the Trade tab, which the
    // optimizer overwrites with each champion whether or not the live engine
    // accepted it — so the two disagree exactly when a proposal was rejected,
    // and reading the terminal's copy reports a set that is not trading.
    std::map<std::string, double> params;
    // This position came from the broker on connect (hot restart / lineup
    // swap), not from a fill of ours, and its strategy is PAUSED until it goes
    // flat. The adopted broker-side stop/TP is what exits it — so an adopted
    // position with no working protective order has nothing that will ever
    // close it, and no strategy watching. Surfaced so the terminal can page.
    bool adopted = false;
};

// Live headroom to the session's automated equity halts, so a remote monitor
// can see how close it is to tripping. A limit of 0 = that halt is disarmed.
struct RiskState {
    double daily_loss = 0;          // equity lost since session start (>0 = down)
    double daily_loss_limit = 0;    // halt when daily_loss reaches this ($)
    double drawdown_pct = 0;        // fraction below the session equity high
    double drawdown_limit_pct = 0;  // halt when drawdown_pct reaches this
    int stale_feed_sec = 0;         // stale-feed halt threshold (with a position)
};

struct LiveSnapshot {
    bool running = false, halted = false;
    // False on a reconciling (TWS) route until the broker has replayed its
    // positions/orders/cash (ReconcileEnd) or the 10s failsafe fired; true from
    // the first publish on non-reconciling routes (sim/ibkr). The journal reads
    // this to anchor a session's PnL baseline to the *real* account equity
    // rather than the pre-reconciliation placeholder.
    bool reconciled = false;
    double cash = 0, equity = 0;
    std::vector<SymbolState> symbols;
    std::vector<OrderRecord> orders;   // newest last, capped
    // Complete per-(symbol, cause) refusal counts for the session. See
    // RefusalStat for what healthy and broken look like.
    std::vector<RefusalStat> refusals;
    // ---- entry-gate state (LiveConfig::entry_gate) --------------------------
    // Published every frame so the gate cannot fail silently, which is this
    // project's recurring defect (oldest_history_age_ms pinned at 0 through a
    // five-hour outage; data.connected true against a login modal).
    //
    //   entry_gate_armed  false = NO gate is installed and strategies may enter
    //                     at any hour, i.e. the 2026-08-13 behaviour. True in
    //                     any session the app started. Reading false on a live
    //                     session IS the alarm.
    //   entry_gate_open   true only while entries are permitted. On a normal
    //                     weekday it is true 09:30-16:00 local and false either
    //                     side; a value of true at 18:00, on a Sunday, or on
    //                     Thanksgiving means the calendar behind it is wrong.
    //   entry_gate_blocked  how many entries the gate refused this session. 0 on
    //                     a healthy day. Nonzero is the number that was
    //                     unobtainable before 0.23.0: strategies signalling into
    //                     a shut exchange, which used to show up only as an IB
    //                     reject with no reason - or, at 16:05, not at all.
    bool entry_gate_armed = false;
    bool entry_gate_open = false;
    uint64_t entry_gate_blocked = 0;
    RiskState risk;
    uint64_t ticks = 0, dropped_ticks = 0;
    int64_t last_tick_ts_ms = 0;       // most recent tick across any symbol
    // Tick -> order submit latency for this session (strategy orders only).
    int64_t lat_p50 = 0, lat_p99 = 0, lat_max = 0;
    uint64_t lat_count = 0;
};

class Engine {
public:
    Engine();
    ~Engine();

    // Strategy is caller-owned and must outlive the run; on_init must fully
    // reset its state. Returns false if a backtest is already running.
    bool start_backtest(BacktestConfig cfg, IStrategy* strategy);
    // Same contract and result plumbing as start_backtest.
    bool start_replay(ReplayConfig cfg, IStrategy* strategy);

    bool running() const { return running_.load(std::memory_order_relaxed); }
    // True exactly once per finished run.
    bool take_result(BacktestResult& out);
    // Engine/strategy log lines, drained by the UI each frame.
    bool pop_log(std::string& out);
    // Same, but also reports whether the line came from the LIVE session thread
    // rather than a backtest. The two share this queue, and a caller that can
    // only guess from global state (e.g. "is a tournament running?") will
    // misfile live output during the autopilot's backtests - which is most of
    // the trading day.
    bool pop_log(std::string& out, bool& from_live);

    // ---- live paper trading ----
    // One strategy instance per symbol (parallel to cfg.symbols); each is
    // caller-owned and must outlive the run. A symbol's events route only to
    // its own instance. All instances share one portfolio/cash (design v1).
    bool start_live(LiveConfig cfg, std::vector<IStrategy*> strategies);
    // Graceful stop: on_stop, joins the thread. keep_broker_orders=true leaves
    // resting broker orders (adopted stop/TP) live at the gateway for a
    // keep-positions restart to re-adopt — the default cancels them so no order
    // outlives the session watching it.
    void stop_live(bool keep_broker_orders = false);
    bool live_running() const { return live_running_.load(std::memory_order_relaxed); }
    std::vector<std::string> live_symbols() const;
    // Exact per-fill records for the journal, drained by the UI each frame
    // (same pattern as pop_log). Live sessions only.
    struct FillRecord {
        int64_t ts_ns = 0;
        uint64_t order_id = 0;
        uint32_t symbol_id = 0;
        uint8_t side = 0;   // tt::Side
        double qty = 0, price = 0, fee = 0;
    };
    bool pop_fill(FillRecord& out);
    // IPC thread: feed a delayed quote into the live engine (dropped if the
    // symbol isn't part of the running session).
    void push_live_tick(const std::string& symbol, int64_t ts_ms, double price,
                        double day_volume);
    // Real-time feed thread (exactly one producer): push a normalized
    // EngineEvent into the live session. False = ring full, caller counts
    // the drop. Separate ring from push_live_tick so both sources keep
    // single-producer semantics.
    bool push_feed_event(const EngineEvent& ev) { return feed_ring_->try_push(ev); }
    // UI thread: async commands consumed by the engine thread.
    void request_cancel(uint64_t order_id);
    // take_profit/stop_loss > 0: attach bracket exit legs (OCO).
    // limit_price > 0: a limit order at that price instead of market.
    // outside_rth: allow fills in extended hours (requires a limit price).
    void submit_manual(uint32_t symbol_id, bool buy, double qty,
                       double take_profit = 0, double stop_loss = 0,
                       double limit_price = 0, bool outside_rth = false);
    void kill_switch();                     // cancel all + flatten + halt strategy
    LiveSnapshot live_snapshot() const;

    // ---- live hot-swap (autopilot) ----
    // Queue new params — and optionally a replacement strategy instance — for
    // one symbol of the running session. The live thread applies a pending
    // swap only while that symbol is FLAT, then re-inits the (new) instance so
    // it re-reads its params with clean state; a watchdog halt on the symbol
    // is lifted. The instance is caller-owned and must outlive the session
    // (lease it like any live strategy). Latest queued swap per symbol wins.
    // `warmup`: fresh seed bars to replay after the re-init this triggers.
    // on_init clears the strategy's history, so passing none means the symbol
    // restarts cold — pass the same bars the caller would seed a new session
    // with. Empty = keep whatever history the session was started with.
    void update_symbol_params(uint32_t symbol_id, std::map<std::string, double> params,
                              std::vector<Bar> warmup = {});
    // `warmup` as in update_symbol_params: a brand-new instance starts with no
    // history at all, so this one matters even more.
    void swap_symbol_strategy(uint32_t symbol_id, IStrategy* strategy,
                              std::map<std::string, double> params,
                              std::vector<Bar> warmup = {});
    // Re-run a symbol's strategy over fresh history without changing anything
    // else. For when the bars a session wanted at start-up only became
    // available later (the candle cache is memory-only, so it is cold on the
    // very first session after launch).
    void reseed_symbol(uint32_t symbol_id, std::vector<Bar> warmup);

private:
    friend class EngineCtx;
    struct LiveCmd {
        enum : uint8_t { Stop = 1, Cancel, Kill, Manual } type = Stop;
        uint8_t buy = 0;
        uint64_t order_id = 0;
        double qty = 0;
        uint32_t symbol_id = 0;
        double take_profit = 0, stop_loss = 0;   // Manual bracket legs
        double limit_price = 0;    // Manual: >0 = limit order (else market)
        uint8_t outside_rth = 0;   // Manual: 1 = allow fills outside RTH
    };
    void run(BacktestConfig cfg, IStrategy* strategy);
    void run_replay(ReplayConfig cfg, IStrategy* strategy);
    void run_live(LiveConfig cfg, std::vector<IStrategy*> strategies);
    void push_log(std::string line);

    // Heap-allocated: the 4 MiB buffer must never land on a caller's stack.
    using MdRing = SpscRing<EngineEvent, 1 << 16>;
    std::unique_ptr<MdRing> md_ring_ = std::make_unique<MdRing>();
    std::unique_ptr<MdRing> feed_ring_ = std::make_unique<MdRing>();   // real-time feed
    std::thread engine_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> has_result_{false};

    std::mutex result_mu_;
    BacktestResult result_;

    std::mutex log_mu_;
    std::deque<std::pair<std::string, bool>> logs_;   // text, came-from-live

    std::mutex fill_mu_;
    std::deque<FillRecord> fill_feed_;
    void push_fill(const FillRecord& f);

    // ---- live state ----
    using CmdRing = SpscRing<LiveCmd, 1 << 12>;
    // Pending hot-swaps (UI thread queues, live thread applies while flat).
    // Not on the cmd ring: param maps aren't POD. has_swaps_ keeps the live
    // loop's fast path to one relaxed atomic load.
    struct PendingSwap {
        uint32_t symbol_id = 0;
        IStrategy* strategy = nullptr;   // null = params-only update
        std::map<std::string, double> params;
        // Fresh seed bars for the re-init this swap triggers. on_init clears the
        // strategy's history, so without these every params update would reset
        // the warmup to zero — see LiveConfig::symbol_warmup. Empty = re-use
        // whatever the session already had.
        std::vector<Bar> warmup;
        // True: this swap exists ONLY to deliver warmup bars; leave the
        // symbol's current params untouched.
        bool keep_params = false;
    };
    std::mutex swap_mu_;
    std::vector<PendingSwap> pending_swaps_;
    std::atomic<bool> has_swaps_{false};
    void queue_swap(PendingSwap s);

    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::thread live_thread_;
    std::atomic<bool> live_running_{false};
    std::atomic<uint64_t> dropped_ticks_{0};
    mutable std::mutex snap_mu_;
    LiveSnapshot snap_;   // orders kept out-of-band, see snap_orders_
    // Orders are published as an immutable shared vector so the string-heavy
    // copy happens OUTSIDE snap_mu_ on both sides: the engine swaps a pointer
    // under the lock, readers copy from the immutable vector after unlocking.
    // Readers can never stall the engine's publish for more than a few loads.
    std::shared_ptr<const std::vector<OrderRecord>> snap_orders_;
    std::vector<std::string> live_symbol_table_;   // symbol name -> id (index+1)
};

} // namespace tt
