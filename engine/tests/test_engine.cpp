#include "doctest.h"

#include "engine/builtin_sma.h"
#include "engine/engine.h"
#include "engine/exec_sim.h"
#include "engine/portfolio.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace tt;

namespace {
Fill mk_fill(uint32_t sym, Side side, double qty, double price, double fee = 0.0) {
    return Fill{1, sym, side, {}, 0, price, qty, fee};
}
} // namespace

TEST_CASE("portfolio: long round trip books realized pnl and fees") {
    Portfolio pf(10'000.0);
    pf.apply(mk_fill(1, Side::Buy, 100, 10.0, 1.0));
    CHECK(pf.cash() == doctest::Approx(10'000 - 1'000 - 1.0));
    pf.mark(1, 12.0);
    CHECK(pf.equity() == doctest::Approx(8'999 + 1'200));
    CHECK(pf.position(1).unrealized_pnl == doctest::Approx(200.0));

    pf.apply(mk_fill(1, Side::Sell, 100, 12.0, 1.0));
    CHECK(pf.position(1).qty == doctest::Approx(0.0));
    CHECK(pf.position(1).realized_pnl == doctest::Approx(200.0));
    CHECK(pf.cash() == doctest::Approx(10'000 - 1'001 + 1'200 - 1.0));
    CHECK(pf.wins() == 1);
    CHECK(pf.losses() == 0);
}

TEST_CASE("portfolio: averaging and position flip") {
    Portfolio pf(100'000.0);
    pf.apply(mk_fill(1, Side::Buy, 100, 10.0));
    pf.apply(mk_fill(1, Side::Buy, 100, 20.0));
    CHECK(pf.position(1).avg_price == doctest::Approx(15.0));

    // Sell 300 @ 18: closes 200 (realized (18-15)*200 = 600), opens 100 short @ 18.
    pf.apply(mk_fill(1, Side::Sell, 300, 18.0));
    CHECK(pf.position(1).qty == doctest::Approx(-100.0));
    CHECK(pf.position(1).avg_price == doctest::Approx(18.0));
    CHECK(pf.position(1).realized_pnl == doctest::Approx(600.0));
}

TEST_CASE("exec sim: latency gates fills; limits fill on crossing prices") {
    ExecParams p;
    p.latency_ns = 1'000'000;  // 1 ms
    p.latency_jitter_ns = 0;
    p.slippage_bps = 0.0;
    p.fee_per_share = 0.0;
    p.min_fee = 0.0;
    ExecSim ex(p);
    std::vector<Fill> fills;

    const uint64_t id = ex.submit(OrderRequest{1, Side::Buy, OrdType::Market, {}, 100, 0}, 0);
    CHECK(id != 0);
    ex.on_price(1, 50.0, 500'000, fills);        // before latency elapses
    CHECK(fills.empty());
    ex.on_price(1, 51.0, 2'000'000, fills);      // after
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == doctest::Approx(51.0));

    fills.clear();
    ex.submit(OrderRequest{1, Side::Buy, OrdType::Limit, {}, 100, 48.0}, 2'000'000);
    ex.on_price(1, 49.0, 4'000'000, fills);      // above limit: no fill
    CHECK(fills.empty());
    ex.on_price(1, 47.5, 5'000'000, fills);      // crossed: fills at market price
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == doctest::Approx(47.5));

    fills.clear();
    const uint64_t c = ex.submit(OrderRequest{1, Side::Sell, OrdType::Limit, {}, 100, 60.0}, 0);
    CHECK(ex.cancel(c));
    ex.on_price(1, 65.0, 10'000'000, fills);
    CHECK(fills.empty());                        // cancelled order never fills
}

namespace {
// Synthetic series with clean trends so the SMA strategy must trade.
std::vector<Bar> synthetic_bars(int n) {
    std::vector<Bar> bars;
    bars.reserve(n);
    const int64_t day_ns = 86'400'000'000'000;
    double px = 100.0;
    for (int i = 0; i < n; ++i) {
        const double trend = std::sin(i / 15.0) * 1.2;   // ~5 regime cycles in 500 bars
        const double wiggle = std::sin(i * 1.7) * 0.3;
        px = std::max(5.0, px + trend + wiggle);
        Bar b{};
        b.ts_ns = int64_t{1'600'000'000'000'000'000} + int64_t{i} * day_ns;
        b.open = px - 0.2;
        b.high = px + 0.5;
        b.low = px - 0.5;
        b.close = px;
        b.volume = 1e6;
        bars.push_back(b);
    }
    return bars;
}

BacktestResult run_backtest_blocking(Engine& eng, const BacktestConfig& cfg, IStrategy* s) {
    REQUIRE(eng.start_backtest(cfg, s));
    BacktestResult res;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!eng.take_result(res)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return res;
}
} // namespace

TEST_CASE("backtest: SMA trades on synthetic data and reruns are bit-identical") {
    Engine eng;
    SmaCrossover sma;

    BacktestConfig cfg;
    cfg.symbol = "TEST";
    cfg.bars = synthetic_bars(500);
    cfg.initial_cash = 100'000.0;
    cfg.params = {{"fast", 5}, {"slow", 20}, {"qty", 100}};

    const BacktestResult a = run_backtest_blocking(eng, cfg, &sma);
    CHECK(a.trades > 4);                       // regime changes force crossovers
    CHECK(a.events == cfg.bars.size() * 5 + 1); // 4 ticks + 1 bar each, + End
    CHECK(a.final_equity > 0.0);
    CHECK(a.lat_count == static_cast<uint64_t>(a.trades));

    const BacktestResult b = run_backtest_blocking(eng, cfg, &sma);
    CHECK(a.trades == b.trades);
    CHECK(a.final_equity == b.final_equity);   // bit-identical, not Approx
    CHECK(a.total_return == b.total_return);
    CHECK(a.max_drawdown == b.max_drawdown);
    REQUIRE(a.fills.size() == b.fills.size());
    for (size_t i = 0; i < a.fills.size(); ++i) {
        CHECK(a.fills[i].ts_ns == b.fills[i].ts_ns);
        CHECK(a.fills[i].price == b.fills[i].price);
        CHECK(a.fills[i].qty == b.fills[i].qty);
    }
}
