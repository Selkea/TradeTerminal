// The optimizer's scoring guard: a parameter set that barely trades is not a
// good one, it is an unmeasured one. Before this, a zero-trade backtest scored
// a perfectly ordinary 0.0 on every metric — and on "Max drawdown", which is
// MINIMISED, that made it the unbeatable global optimum.
#include "doctest.h"

#include "panels/sweep.h"
#include "symbol_params.h"   // session_bars / time_stop_reachable: the LIVE half

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

using namespace tt;
using tt::ui::bar_seconds_of_interval;
using tt::ui::sweep_eod_flatten_h;

// ---- which parameters the optimizer may touch ------------------------------
TEST_CASE("sweep: sizing and session-shape parameters are never swept") {
    using tt::ui::sweep_param_is_fixed;
    // Sizing: optimizing it just maximizes leverage.
    CHECK(sweep_param_is_fixed("qty"));
    CHECK(sweep_param_is_fixed("max_qty"));
    CHECK(sweep_param_is_fixed("budget_pct"));
    CHECK(sweep_param_is_fixed("alloc_pct"));   // pre-SDK-v3 name
    CHECK(sweep_param_is_fixed("risk_pct"));
    // Session shape: a policy choice or a fact about the market. session_min /
    // eod_min were the leak — ORB's session window, missing from the list, and
    // the optimizer had SNXX trading a 155-minute day live.
    CHECK(sweep_param_is_fixed("enter_from_h"));
    CHECK(sweep_param_is_fixed("enter_until_h"));
    CHECK(sweep_param_is_fixed("session_min"));
    CHECK(sweep_param_is_fixed("eod_min"));
    // Cost floor: a measurement of what the broker charges. Swept, it inverts
    // into a profit target — on 2026-08-10 all 13 winning rsi2_pullback runs
    // came back 69.93-100 c/share (SNXX pinned at exactly 100 in 5 of 6) on a
    // $7 stock whose measured round-trip cost was ~1.05 c/share.
    CHECK(sweep_param_is_fixed("min_gain_cps"));
    // Signal parameters must still be swept, or the optimizer does nothing.
    CHECK_FALSE(sweep_param_is_fixed("entry_z"));
    CHECK_FALSE(sweep_param_is_fixed("trend_len"));
    CHECK_FALSE(sweep_param_is_fixed("range_min"));
    CHECK_FALSE(sweep_param_is_fixed("tp_r"));
    CHECK_FALSE(sweep_param_is_fixed("rsi_len"));
}
using namespace tt::ui;

namespace {
BacktestResult mk(int trades, double sharpe, double dd) {
    BacktestResult r;
    r.trades = trades;
    r.sharpe = sharpe;
    r.max_drawdown = dd;
    r.total_return = sharpe;   // stand-in for the other maximised metrics
    r.win_rate = 0.6;
    return r;
}
constexpr int kSharpe = 0, kReturn = 1, kMaxDD = 2, kWinRate = 3;
}  // namespace

TEST_CASE("a well-sampled run is scored on its real metric") {
    const BacktestResult r = mk(50, 1.75, 0.08);
    CHECK(sweep_metric_of(r, kSharpe, kSweepMinTrades) == doctest::Approx(1.75));
    CHECK(sweep_metric_of(r, kMaxDD, kSweepMinTrades) == doctest::Approx(0.08));
}

TEST_CASE("a zero-trade run loses on every maximised metric") {
    const BacktestResult r = mk(0, 0.0, 0.0);
    for (int m : {kSharpe, kReturn, kWinRate}) {
        const double v = sweep_metric_of(r, m, kSweepMinTrades);
        CHECK(std::isinf(v));
        CHECK(v < 0.0);
        // It must lose to a genuinely bad but real result.
        CHECK(v < sweep_metric_of(mk(50, -3.66, 0.4), m, kSweepMinTrades));
    }
}

TEST_CASE("a zero-trade run no longer wins the minimised metric") {
    // This is the dangerous one: 0.0 drawdown is the best score achievable, so
    // without the guard "never open a position" is the global optimum.
    const double none = sweep_metric_of(mk(0, 0.0, 0.0), kMaxDD, kSweepMinTrades);
    const double real = sweep_metric_of(mk(50, 1.2, 0.15), kMaxDD, kSweepMinTrades);
    CHECK(std::isinf(none));
    CHECK(none > 0.0);          // +inf, because lower is better here
    CHECK(real < none);         // the real run wins
}

TEST_CASE("the floor is a floor, not a preference") {
    // One trade below the floor is rejected; exactly at the floor is kept.
    CHECK(std::isinf(sweep_metric_of(mk(kSweepMinTrades - 1, 9.9, 0.01),
                                     kSharpe, kSweepMinTrades)));
    CHECK(sweep_metric_of(mk(kSweepMinTrades, 9.9, 0.01), kSharpe, kSweepMinTrades) ==
          doctest::Approx(9.9));
}

TEST_CASE("the holdout floor is lower than the training floor") {
    // The holdout is only the tail slice of the window, so it cannot be held to
    // the same count - but it must still be non-zero.
    CHECK(kSweepMinHoldoutTrades > 0);
    CHECK(kSweepMinHoldoutTrades < kSweepMinTrades);
    const BacktestResult r = mk(kSweepMinHoldoutTrades, 0.9, 0.1);
    CHECK(sweep_metric_of(r, kSharpe, kSweepMinHoldoutTrades) == doctest::Approx(0.9));
    CHECK(std::isinf(sweep_metric_of(r, kSharpe, kSweepMinTrades)));
}

TEST_CASE("the legacy two-arg form keeps its old, opinion-free behaviour") {
    const BacktestResult r = mk(0, 0.0, 0.0);
    CHECK(sweep_metric_of(r, kSharpe) == doctest::Approx(0.0));
    CHECK(sweep_metric_of(r, kMaxDD) == doctest::Approx(0.0));
}

// ---- the sweep may not propose a hold the trading day cannot contain -------
//
// 2026-08-14. The tournament fitted STKH's bollinger_reversion time_stop to
// 233 bars on a 300 s series and scored it +8.60%, because Engine::run replayed
// six months with no day boundary. Live, everything is force-flattened at 15:57,
// so 77 bars is the ceiling — and BollRev places no price stop, making that time
// stop the ONLY thing that would ever close a losing position. It could not fire
// from the moment it was crowned. -$506, 85% of the day's loss.

TEST_CASE("sweep: time_stop is capped to what the bar size can reach in a day") {
    using tt::ui::sweep_param_max;
    // bollinger_reversion declares max 500; rsi2_pullback the same. On the
    // 5-minute series both are traded on, the reachable maximum is 77.
    CHECK(sweep_param_max("time_stop", 500, 300) == doctest::Approx(77));
    CHECK(sweep_param_max("time_stop", 500, 60) == doctest::Approx(387));
    // 233 bars is a legitimate swing hold on DAILY bars, so the declared range
    // is left alone there. The bound is a fact about the intraday session, which
    // is exactly why it cannot live in the strategy's bar-size-agnostic
    // ParamDesc — it has to be applied where the interval is known.
    CHECK(sweep_param_max("time_stop", 500, 86400) == doctest::Approx(500));
    // Never RAISES a declared maximum: a strategy that deliberately allows only
    // 20 bars keeps its 20.
    CHECK(sweep_param_max("time_stop", 20, 300) == doctest::Approx(20));
    // Only time_stop, and only when the bar size is known. Capping a lookback
    // or a z-threshold at "bars in a day" would be nonsense, and inventing a
    // limit for an interval nobody recognises is worse than the blind spot.
    CHECK(sweep_param_max("length", 300, 300) == doctest::Approx(300));
    CHECK(sweep_param_max("trend_len", 1000, 300) == doctest::Approx(1000));
    CHECK(sweep_param_max("time_stop", 500, 0) == doctest::Approx(500));
}

TEST_CASE("sweep: the cap agrees with the live admission gate, exactly") {
    // Two files answer "how many bars fit in a trading day" — sweep.h for the
    // fit and symbol_params.h for the session that trades it. If they ever
    // disagree, the optimizer proposes sets the session then refuses, and every
    // lineup build dies in the EXCLUDED path with nothing left to trade.
    // Pinned rather than shared, because sweep.h must stay free of the
    // terminal's calendar (it is included by the engine-facing panel code).
    for (int bar_sec : {60, 300, 900, 3600}) {
        CHECK(tt::ui::sweep_param_max("time_stop", 100000, bar_sec) ==
              doctest::Approx(tt::ui::session_bars(bar_sec)));
        // ...and the boundary itself round-trips: the largest value the sweep
        // may propose is the largest value the session will accept.
        const int cap = tt::ui::session_bars(bar_sec);
        CHECK(tt::ui::time_stop_reachable({{"time_stop", double(cap)}}, bar_sec));
        CHECK_FALSE(
            tt::ui::time_stop_reachable({{"time_stop", double(cap) + 1}}, bar_sec));
    }
}

TEST_CASE("sweep: interval strings map to the bar size the fit is traded on") {
    using tt::ui::bar_seconds_of_interval;
    CHECK(bar_seconds_of_interval("5m") == 300);
    CHECK(bar_seconds_of_interval("1h") == 3600);
    // "1d" is one RTH SESSION, not 86400: a daily bar spans the trading day, and
    // pricing it at 24 h would understate how many of them fit in one.
    CHECK(bar_seconds_of_interval("1d") == 23400);
    CHECK(bar_seconds_of_interval("weekly") == 0);   // unknown: do not judge
    CHECK(bar_seconds_of_interval("") == 0);
}

// ---------------------------------------------------------------------------
// THE ONE LINE THE WHOLE DAY BOUNDARY HANGS ON.
//
// BacktestConfig::eod_flatten_h is what stops the optimizer scoring holds that
// production force-flattens out of existence (the 2026-08-14 STKH time_stop of
// 233 bars, +8.60% in backtest, -$506 live). But App::pump_sweep is the ONLY
// place in the repo that ever sets it non-zero, nothing constructs an App in a
// test, and every case that exercises the field hand-sets it. A parity audit
// found that deleting the arming line disarmed the entire fix with a green
// suite. These pin the decision and the fact that app.cpp still asks for it.

TEST_CASE("sweep: intraday bars are scored with the live day boundary") {
    CHECK(sweep_eod_flatten_h(300) == doctest::Approx(kEodBackstopH));    // 5m
    CHECK(sweep_eod_flatten_h(3600) == doctest::Approx(kEodBackstopH));   // 1h
    CHECK(sweep_eod_flatten_h(60) == doctest::Approx(kEodBackstopH));
}

TEST_CASE("sweep: a session-length bar has no intraday boundary to honour") {
    // "1d" is 23400 s — one RTH session, NOT 86400. Scoring a daily series
    // against a 15:57 cutoff would be the bug: a daily bar's clock is the
    // data's, not the world's (BacktestConfig::eod_flatten_h).
    CHECK(sweep_eod_flatten_h(bar_seconds_of_interval("1d")) == doctest::Approx(0.0));
    CHECK(sweep_eod_flatten_h(86'400) == doctest::Approx(0.0));
}

TEST_CASE("sweep: an unrecognised interval is left unarmed rather than guessed") {
    CHECK(sweep_eod_flatten_h(bar_seconds_of_interval("30s")) == doctest::Approx(0.0));
    CHECK(sweep_eod_flatten_h(0) == doctest::Approx(0.0));
    CHECK(sweep_eod_flatten_h(-1) == doctest::Approx(0.0));
}

TEST_CASE("sweep: pump_sweep still arms the boundary through that predicate") {
    // Source-text pin. The cases above prove the predicate is right; only this
    // proves the optimizer still calls it. Without it the predicate could be
    // perfect and orphaned, which is the exact state the audit found.
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    CHECK(src.find("sweep_base_.eod_flatten_h = sweep_eod_flatten_h(") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// THE SIZE THE OPTIMIZER SCORES AT.
//
// The boundary cases above are about WHEN the replay trades. These are about
// HOW MUCH — the parity hole that was silently wrong on every score ever
// produced. Engine::run built its EngineCtx with no risk pointer, so
// ctx.budget() returned cash * 0.95: measured at 989 shares / $94,509 of
// $100,000 against a $5,000 live per-symbol budget, roughly 19x.

TEST_CASE("risk: the notional cap is half the daily-loss budget at a 20% move") {
    // The live derivation, now shared with the optimizer instead of inlined in
    // App::start_live where the replay could not reach it.
    RiskLimits rl{};
    rl.daily_max_loss = 2'000.0;
    CHECK(tt::ui::position_notional_cap(rl, 100'000.0) == doctest::Approx(5'000.0));
}

TEST_CASE("risk: an explicit cap is returned untouched, so applying twice is safe") {
    // Live now assigns unconditionally (rl.max_position_notional = cap(...))
    // rather than skipping already-set symbols, so idempotence is load-bearing:
    // a non-idempotent form would re-derive over an operator's explicit number.
    RiskLimits rl{};
    rl.max_position_notional = 250.0;
    rl.daily_max_loss = 2'000.0;   // would derive 5,000 if it were consulted
    CHECK(tt::ui::position_notional_cap(rl, 100'000.0) == doctest::Approx(250.0));
    rl.max_position_notional = tt::ui::position_notional_cap(rl, 100'000.0);
    CHECK(tt::ui::position_notional_cap(rl, 100'000.0) == doctest::Approx(250.0));
}

TEST_CASE("risk: with no daily-loss limit a symbol is still never left uncapped") {
    // Not a guard-rail preference. An absent cap makes ctx.budget() return
    // cash * 0.95, which hands one position the entire account.
    RiskLimits rl{};
    CHECK(tt::ui::position_notional_cap(rl, 100'000.0) == doctest::Approx(20'000.0));
    CHECK(tt::ui::position_notional_cap(rl, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("risk: the sweep scores at the live caps, sized off the live budget") {
    RiskLimits live{};
    live.daily_max_loss = 2'000.0;
    live.max_order_qty = 1'000;
    live.max_position_qty = 5'000;
    const RiskLimits swept = tt::ui::sweep_risk_limits(live, 100'000.0);
    CHECK(swept.max_position_notional == doctest::Approx(5'000.0));
    CHECK(swept.max_order_qty == doctest::Approx(1'000));      // carried through
    CHECK(swept.max_position_qty == doctest::Approx(5'000));
    CHECK(swept.price_band_pct == doctest::Approx(live.price_band_pct));
}

TEST_CASE("risk: the run_live-only halts are dropped rather than carried in dead") {
    // Engine::run has no equity-halt machinery at all, so a BacktestConfig
    // carrying daily_max_loss would read as protection the replay applies and
    // does not. The replay-relevant consequence of the limit survives — it is
    // what sized the notional cap on the line above.
    RiskLimits live{};
    live.daily_max_loss = 2'000.0;
    live.max_drawdown_pct = 0.10;
    live.stale_feed_sec = 45;
    live.disable_auto_halt = true;
    const RiskLimits swept = tt::ui::sweep_risk_limits(live, 100'000.0);
    CHECK(swept.daily_max_loss == doctest::Approx(0.0));
    CHECK(swept.max_drawdown_pct == doctest::Approx(0.0));
    CHECK(swept.stale_feed_sec == 0);
    CHECK_FALSE(swept.disable_auto_halt);
    CHECK(swept.max_position_notional == doctest::Approx(5'000.0));   // still sized
}

TEST_CASE("sweep: pump_sweep still arms the risk through that predicate") {
    // The same source-text pin as the day boundary, for the same reason: nothing
    // constructs an App, so the arming line is unreachable from a test and the
    // predicate above could be perfect and orphaned.
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    CHECK(src.find("sweep_base_.risk = sweep_risk_limits(") != std::string::npos);
    // ...and that live derives its cap from the SAME function. Two copies of the
    // formula is the defect this whole fix is about; if this line ever goes back
    // to an inline expression, the optimizer silently scores at a size live does
    // not use again.
    CHECK(src.find("position_notional_cap(rl, opts.session_cash)") !=
          std::string::npos);
}
