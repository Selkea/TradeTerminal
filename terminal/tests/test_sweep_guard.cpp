// The optimizer's scoring guard: a parameter set that barely trades is not a
// good one, it is an unmeasured one. Before this, a zero-trade backtest scored
// a perfectly ordinary 0.0 on every metric — and on "Max drawdown", which is
// MINIMISED, that made it the unbeatable global optimum.
#include "doctest.h"

#include "panels/sweep.h"

#include <cmath>

using namespace tt;
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
