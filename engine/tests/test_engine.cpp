#include "doctest.h"

#include "engine/broker.h"
#include "engine/engine.h"
#include "engine/events.h"
#include "engine/exec_sim.h"
#include "engine/portfolio.h"
#include "tt/strategy_api.h"
#include "tt/strategy_registry.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
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

    const uint64_t id =
        ex.submit(OrderRequest{1, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0}, 0);
    CHECK(id != 0);
    ex.on_price(1, 50.0, 500'000, fills);        // before latency elapses
    CHECK(fills.empty());
    ex.on_price(1, 51.0, 2'000'000, fills);      // after
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == doctest::Approx(51.0));

    fills.clear();
    ex.submit(OrderRequest{1, Side::Buy, OrdType::Limit, {}, 100, 48.0, 0, 0, 0}, 2'000'000);
    ex.on_price(1, 49.0, 4'000'000, fills);      // above limit: no fill
    CHECK(fills.empty());
    ex.on_price(1, 47.5, 5'000'000, fills);      // crossed: fills at market price
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == doctest::Approx(47.5));

    fills.clear();
    const uint64_t c =
        ex.submit(OrderRequest{1, Side::Sell, OrdType::Limit, {}, 100, 60.0, 0, 0, 0}, 0);
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
    // sma_crossover.cpp is compiled into this test binary as a static-link
    // source (see engine/CMakeLists.txt) -- the same implementation
    // tt_terminal's "" built-in resolves to (see App::acquire_strategy).
    const StaticStrategyEntry* e = find_static_strategy("sma_crossover.cpp");
    REQUIRE(e != nullptr);
    Engine eng;
    IStrategy* sma = e->create();

    BacktestConfig cfg;
    cfg.symbol = "TEST";
    cfg.bars = synthetic_bars(500);
    cfg.initial_cash = 100'000.0;
    cfg.params = {{"fast", 5}, {"slow", 20}, {"qty", 100}};

    const BacktestResult a = run_backtest_blocking(eng, cfg, sma);
    CHECK(a.trades > 4);                       // regime changes force crossovers
    CHECK(a.events == cfg.bars.size() * 5 + 1); // 4 ticks + 1 bar each, + End
    CHECK(a.final_equity > 0.0);
    CHECK(a.lat_count == static_cast<uint64_t>(a.trades));

    const BacktestResult b = run_backtest_blocking(eng, cfg, sma);
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
    sma->destroy();
}

// run_live calls broker->take_reject() for every Rejected event and stores
// whatever comes back. An adapter that captures no reason must yield an empty
// one (code 0, empty msg) so the order simply reads "rejected", never garbage.
// This pins the base-class default that the reference/sim adapters inherit.
namespace {
struct StubBroker : IBrokerAdapter {
    uint64_t submit(const OrderRequest&, int64_t) override { return 0; }
    bool cancel(uint64_t) override { return false; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent&) override { return false; }
    bool ready() const override { return false; }
};
} // namespace

TEST_CASE("broker: default take_reject reports no reason") {
    StubBroker b;
    const RejectReason r = b.take_reject(42);
    CHECK(r.code == 0);
    CHECK(r.message.empty());
}

// ---- hot-restart reconciliation: gate dispatch, hold adopted positions until
// flat, then resume. Drives run_live with a scripted broker + injected ticks.
namespace {
struct FakeReconcileBroker : IBrokerAdapter {
    std::mutex mu;
    std::deque<EngineEvent> q;   // events the engine will drain via poll_event

    void emit(const EngineEvent& e) {
        std::lock_guard l(mu);
        q.push_back(e);
    }
    uint64_t submit(const OrderRequest&, int64_t) override { return 1; }
    bool cancel(uint64_t) override { return true; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent& out) override {
        std::lock_guard l(mu);
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    bool ready() const override { return true; }
    bool reconciles() const override { return true; }
};

// Counts per-instance dispatch + on_init so a test can observe gate/hold/resume.
struct RecordingStrat : IStrategy {
    std::atomic<int> inits{0};
    std::atomic<int> ticks{0};
    void on_init(IStrategyContext&) noexcept override { ++inits; }
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override { ++ticks; }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

EngineEvent ev_pos(uint32_t sid, double qty, double avg) {
    EngineEvent e{};
    e.type = static_cast<uint16_t>(EvType::PosSnap);
    e.symbol_id = sid;
    e.u.pos.qty = qty;
    e.u.pos.avg_price = avg;
    return e;
}
EngineEvent ev_order(uint32_t sid, uint64_t id, double qty, Side side, OrdType t, double px) {
    EngineEvent e{};
    e.type = static_cast<uint16_t>(EvType::OrderNew);
    e.symbol_id = sid;
    e.u.order.order_id = id;
    e.u.order.qty = qty;
    e.u.order.limit_price = px;
    e.u.order.side = static_cast<uint8_t>(side);
    e.u.order.ord_type = static_cast<uint8_t>(t);
    return e;
}
EngineEvent ev_acct(double cash) {
    EngineEvent e{};
    e.type = static_cast<uint16_t>(EvType::AcctSnap);
    e.u.acct.cash = cash;
    return e;
}
EngineEvent ev_reconcile_end() {
    EngineEvent e{};
    e.type = static_cast<uint16_t>(EvType::ReconcileEnd);
    return e;
}
EngineEvent ev_fill(uint32_t sid, uint64_t id, Side side, double qty, double px) {
    EngineEvent e{};
    e.type = static_cast<uint16_t>(EvType::Fill);
    e.symbol_id = sid;
    e.u.fill.order_id = id;
    e.u.fill.qty = qty;
    e.u.fill.price = px;
    e.u.fill.side = static_cast<uint8_t>(side);
    return e;
}

// Pump a few ticks per poll, waiting up to `ms` for pred() to hold.
template <class Pred>
bool pump_until(Engine& eng, Pred pred, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);
        eng.push_live_tick("BBB", 1, 20.0, 0.0);
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return pred();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
} // namespace

TEST_CASE("live reconciliation: gate dispatch, hold until flat, then resume") {
    Engine eng;
    FakeReconcileBroker broker;
    RecordingStrat held;   // AAA (sid 1): comes back holding a position
    RecordingStrat flat;   // BBB (sid 2): flat, should trade once reconciled

    // Adopt state WITHOUT ending reconciliation yet: AAA long 100 with a resting
    // protective stop; cash. No PosSnap for BBB (it's flat).
    broker.emit(ev_pos(1, 100.0, 50.0));
    broker.emit(ev_order(1, 5001, 100.0, Side::Sell, OrdType::Stop, 45.0));
    broker.emit(ev_acct(100'000.0));

    LiveConfig cfg;
    cfg.symbols = {"AAA", "BBB"};
    cfg.broker = &broker;
    cfg.bar_seconds = 100'000;   // keep bars from firing; assert on on_tick only
    REQUIRE(eng.start_live(cfg, {&held, &flat}));

    // Phase A — gated: reconciliation hasn't ended, so NO symbol is dispatched
    // even as ticks flow. The adopted position is already seeded.
    REQUIRE(pump_until(eng, [&] {
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0);
    }));
    for (int i = 0; i < 50; ++i) {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);
        eng.push_live_tick("BBB", 1, 20.0, 0.0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(flat.ticks.load() == 0);
    CHECK(held.ticks.load() == 0);

    // Phase B — reconciliation ends: the flat symbol trades; the symbol holding
    // an adopted position stays paused.
    broker.emit(ev_reconcile_end());
    CHECK(pump_until(eng, [&] { return flat.ticks.load() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(held.ticks.load() == 0);   // still held — it has a position

    // Phase C — the adopted stop fills, flattening AAA: its strategy re-inits and
    // resumes. (on_init: 1 at start + 1 on resume.)
    const int held_inits_before = held.inits.load();
    broker.emit(ev_fill(1, 5001, Side::Sell, 100.0, 45.0));
    REQUIRE(pump_until(eng, [&] {
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(0.0);
    }));
    CHECK(pump_until(eng, [&] { return held.ticks.load() > 0; }));   // resumed
    CHECK(held.inits.load() == held_inits_before + 1);

    eng.stop_live();
}
