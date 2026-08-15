#pragma once

#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace tt::ui {

// Parameters the optimizer must NOT sweep, whatever a strategy declares.
//
// Two kinds, both of which the sweep would happily ruin:
//
//  - SIZING (qty, max_qty, budget_pct, risk_pct). Not signal. Optimizing size
//    just maximizes leverage — the simulator obliges, the risk manager doesn't.
//    (alloc_pct is budget_pct's pre-SDK-v3 name; kept so a value left in an old
//    config can't be swept either.)
//  - SESSION SHAPE (enter_from_h, enter_until_h, session_min, eod_min). A
//    policy choice or a fact about the market, not something to discover from
//    price. Sweeping a trading window overfits to whatever slice the backtest
//    got lucky on and then barely trades live.
//  - COST FLOORS (min_gain_cps). A measurement of what the broker charges, not
//    a knob. Swept, it stops being a cost floor and becomes a profit target —
//    and the optimizer always wants a bigger one, because in-sample the trades
//    that clear a high bar are exactly the winners. On 2026-08-10 all 13
//    winning rsi2_pullback runs came back between 69.93 and 100 c/share, SNXX
//    pinned at exactly 100 in 5 of 6 runs: a $1.00-per-share minimum gain on a
//    $7 stock, against a measured round-trip cost of ~1.05 c/share. The gate
//    then never opens, so every position leaves on the time stop instead.
//
// session_min/eod_min were the leak: they are ORB's session window, they were
// missing from this list, and on 2026-08-05 SNXX was live with session_min
// 185.45 / eod_min 30.68 — an optimizer-invented 155-minute day that stopped
// trading the symbol around noon. Worse in the other direction: session_min
// swept ABOVE the real session means the bar-count EOD flatten never trips and
// the position rides overnight (orb_breakout carries a clock backstop for
// exactly that, but the parameter should never have been tunable).
inline bool sweep_param_is_fixed(const std::string& n) {
    return n == "qty" || n == "max_qty" || n == "budget_pct" ||
           n == "alloc_pct" || n == "risk_pct" || n == "enter_from_h" ||
           n == "enter_until_h" || n == "session_min" || n == "eod_min" ||
           n == "min_gain_cps";
}

// Bar size in seconds for an optimizer interval string ("5m" / "1h" / "1d" —
// sweep_interval_str's table). 0 for anything unrecognised, which every caller
// reads as "do not judge".
inline int bar_seconds_of_interval(const std::string& ivl) {
    if (ivl == "5m") return 300;
    if (ivl == "1h") return 3600;
    if (ivl == "1d") return 6 * 3600 + 30 * 60;   // one RTH session
    return 0;
}

// The upper bound the optimizer may actually sweep a parameter to, given the bar
// size the fit will be traded on.
//
// Only time_stop, and only downwards. 2026-08-14: the tournament fitted STKH's
// time_stop to 233 bars on a 300 s series and scored it +8.60%, because the
// backtest replayed six months with no day boundary. Live, the engine flattens
// everything at 15:57, so 77 bars is the most a position can ever hold — and
// bollinger_reversion places no price stop, making that time stop the ONLY thing
// that can close a losing position. The fit was unreachable the moment it was
// crowned. See tt::ui::time_stop_reachable for the full post-mortem.
//
// The cap lives HERE, at the sweep request, and NOT in the strategy's ParamDesc
// (bollinger_reversion.cpp declares max 500; rsi2_pullback the same). A
// ParamDesc is bar-size agnostic and 233 bars is a perfectly legitimate swing
// hold on daily bars — it is only impossible on intraday bars that get force
// flattened every afternoon. The bound is a fact about the SESSION, so it
// belongs where the session's bar size is known.
//
// bar_seconds 0 (an interval nobody recognises) leaves the declared max alone:
// refusing to sweep on an unknown bar size would be inventing a limit rather
// than applying one.
inline double sweep_param_max(const std::string& name, double desc_max,
                              int bar_seconds) {
    if (name != "time_stop" || bar_seconds <= 0) return desc_max;
    // Mirrors tt::ui::session_bars (terminal/src/symbol_params.h): 09:30 to the
    // 15:57 EOD flatten. Restated rather than included so this header stays free
    // of the terminal's calendar; test_sweep_guard pins the two together at
    // every bar size, because a disagreement means the optimizer proposes sets
    // the live session then refuses and every lineup build dies EXCLUDED.
    //
    // INTEGER seconds, not (15.95 - 9.5) * 3600: 15.95 has no exact double and
    // that expression is 23219.999999999996, one bar short at every size that
    // divides the day evenly (386 instead of 387 at 60 s).
    const int span_s = 15 * 3600 + 57 * 60 - (9 * 3600 + 30 * 60);   // 23220
    const int reachable = span_s / bar_seconds;
    if (reachable < 1) return desc_max;   // bar longer than a session: not ours to judge
    const double cap = static_cast<double>(reachable);
    return desc_max < cap ? desc_max : cap;
}

// Automatic optimizer: coordinate descent over a strategy's declared
// parameters — each pass sweeps every param 1-D across its range (others
// fixed at the best so far), a second pass refines in a narrower window,
// and the winner is applied back to the strategy's parameters.
class SweepPanel {
public:
    struct Request {
        std::string strat_key;   // strategy to optimize ("" = built-in SMA)
        std::string symbol, interval, range;
        double cash = 100'000.0;
        int metric = 0;          // index into kSweepMetrics
        // Walk-forward guard: optimize on the first (100-h)% of the data,
        // then score the winner on the held-out tail it never saw. 0 = off.
        double holdout_pct = 0;
    };

    // Owned by App (which runs the cells); read by the panel every frame.
    struct State {
        bool running = false;
        int done = 0, total = 0;      // backtests finished / planned
        int metric = 0;
        std::string label;            // "AAPL 1d 2y — SmaCrossover"
        // Current 1-D param sweep (for the live plot + progress line).
        std::string cur_param;
        int pass = 0, n_passes = 0;
        std::vector<double> xs, vals;
        // Result: best parameter set, its in-sample metric, applied or not.
        std::map<std::string, double> best;
        double best_metric = 0;
        bool has_best = false;
        bool applied = false;         // best written into the strategy's params
        double holdout_pct = 0;       // >0: holdout run follows the passes
        bool has_holdout = false;     // holdout_val is valid
        double holdout_val = 0;       // winner's metric on unseen data
        int holdout_trades = 0;       // sample size behind holdout_val

        // Strategy tournament (auto-pick): every loaded strategy is optimized
        // on the same data; the champion (best holdout score) is applied.
        struct TourneyRow {
            std::string name;
            double score = 0;
            bool holdout = false;     // score came from unseen data
            bool valid = false;
            bool champion = false;
        };
        struct Tourney {
            bool active = false;
            int idx = 0, total = 0;
            std::string current;      // candidate being optimized (display name)
            std::string symbol;
            bool done = false;        // rows hold the final ranking
            std::vector<TourneyRow> rows;
        } tourney;
    };

    using RunFn = std::function<void(const Request&)>;
    // Same request; the app optimizes EVERY loaded strategy and applies the
    // champion (strat_key in the request is ignored).
    using TournamentFn = std::function<void(const Request&)>;
    using CancelFn = std::function<void()>;
    // Resolve a strategy key to its display name / current param values.
    using NameFn = std::function<std::string(const std::string& key)>;
    using ParamsFn =
        std::function<std::map<std::string, double>(const std::string& key)>;

    explicit SweepPanel(Engine& eng) : eng_(eng) {}
    // strat_keys: selectable strategies (loaded modules; built-in added in the
    // UI). The swept parameter names come from the picked strategy's params.
    void draw(bool* open, const std::vector<std::string>& strat_keys, const NameFn& name,
              const ParamsFn& params_of, const State& st, const RunFn& run,
              const TournamentFn& tournament, const CancelFn& cancel);

    // Session persistence.
    struct Settings {
        std::string strat_key, symbol;
        int interval_idx = 2, range_idx = 3;
        double cash = 100'000.0;
        int metric = 0;
        bool holdout = true;
        double holdout_pct = 25.0;
    };
    Settings settings() const {
        return {strat_key_, sym_,     interval_idx_, range_idx_,
                cash_,      metric_,  use_holdout_,  holdout_pct_};
    }
    void restore(const Settings& s);

private:
    Engine& eng_;
    std::string strat_key_;   // "" = built-in SMA
    char sym_[16] = "AAPL";
    int interval_idx_ = 2;   // 1d
    int range_idx_ = 3;      // 2y
    double cash_ = 100'000.0;
    int metric_ = 0;
    double holdout_pct_ = 25.0;
    bool use_holdout_ = true;
};

// Auto-optimizer shape: passes over the params, steps per 1-D sweep. Pass 2+
// refines in a window this fraction of the full range, centered on the best.
constexpr int kSweepPasses = 2;
constexpr int kSweepSteps = 12;
constexpr double kSweepRefineWindow = 0.25;

// How long App::pump_sweep may spend draining finished backtests in one TICK
// (App::tick, ~10 ms when not rendering, ~30 ms behind vsync when it is).
//
// It used to take exactly ONE result per frame, and main.cpp sets
// glfwSwapInterval(1), so the optimizer advanced one cell per vsync: ~30 ms of
// wall clock per cell against a measured 5.84 ms of engine time (582 backtests,
// 3398 ms total, 2026-08-10 lineup build). The optimizer ran at the refresh rate
// of a monitor nobody was looking at.
//
// 8 ms, chosen against that 5.84 ms mean rather than as a round number: a budget
// SHORTER than one backtest collects at most one result per tick and leaves the
// ceiling exactly where it was, just at 16.7 ms instead of 30. 8 ms clears the
// mean with margin, and still leaves half of a 16.7 ms frame for the UI — which
// only matters at all while a sweep is running.
constexpr int64_t kSweepDrainBudgetMs = 8;

// How long one tournament candidate may wait for its bars, and how long the
// whole tournament for one symbol may take. Seconds, on App::mono_s().
//
// mono_s(), NOT ImGui::GetTime(): App::pump_tournament runs from App::tick(),
// where no frame exists. ImGui's clock only advances inside NewFrame, so on the
// frame clock a minimized window would pause a tournament's budget outright and
// then expire it in one unclamped step on restore. These are real-world
// deadlines and must keep time whether or not anyone is looking (see
// tick_clock.h for the 2026-08-11 incident).
//
// THE FETCH BUDGET, from the measurements in net/hist_pacing.h rather than a
// round number: a successful fetch is median 6 s and max 17 s; a silently
// unanswered one is classified dead at 20 s and retried once, giving it another
// 20 s; and net/hist_pacing.h may now hold a send back for a few seconds of
// deliberate spacing. 20 + 20 + spacing is already ~45 s, so the old flat 60 s
// left almost no margin for a fetch that legitimately died once and recovered.
// 75 s covers the whole legitimate path with room to spare, and it is no longer
// the common exit anyway: a fetch the feed ERRORS out is now settled within a
// frame (App::sweep_fetch_pending), which is what the wait used to be spent on.
constexpr double kTournFetchTimeoutS = 75.0;
// The hard wall for one symbol. A healthy tournament is ~12 s once the bar cache
// removes the duplicate fetches (one fetch, then four cache hits, and five
// sweeps of ~110 backtests at ~5.84 ms each), so this is an order of magnitude
// of headroom. It exists because the failure mode is not slowness, it is a data
// session that has stopped answering — and on 2026-08-10 that ran unbounded for
// five minutes per symbol, six symbols deep, with nothing to stop it.
constexpr double kTournDeadlineS = 180.0;

// Higher is better for every metric except max drawdown.
constexpr const char* kSweepMetrics[] = {"Sharpe", "Return", "Max drawdown", "Win rate"};
inline bool sweep_metric_minimize(int m) { return m == 2; }

// A parameter set that barely trades is not a good parameter set — it is an
// unmeasured one. Every metric here degenerates on a thin sample: Sharpe and
// win rate are computed from a handful of returns, and "Max drawdown" is
// MINIMISED, so a run that never opens a position scores a flat 0.0 and is the
// unbeatable global optimum. Require a sample proportional to the window, with
// a hard floor, and score anything thinner as the worst possible value.
constexpr int kSweepMinTrades = 20;
// The holdout is only the last slice of the window (holdout_pct, default 25%),
// so it gets a proportionally smaller floor - but still not zero.
constexpr int kSweepMinHoldoutTrades = 5;
inline double sweep_reject_score(int m) {
    return sweep_metric_minimize(m) ? std::numeric_limits<double>::infinity()
                                    : -std::numeric_limits<double>::infinity();
}
inline double sweep_metric_of(const BacktestResult& r, int m, int min_trades) {
    if (r.trades < min_trades) return sweep_reject_score(m);
    switch (m) {
    case 1: return r.total_return;
    case 2: return r.max_drawdown;
    case 3: return r.win_rate;
    default: return r.sharpe;
    }
}
// Back-compat overload: no sample-size opinion. Prefer the 3-arg form.
inline double sweep_metric_of(const BacktestResult& r, int m) {
    return sweep_metric_of(r, m, 0);
}

} // namespace tt::ui
