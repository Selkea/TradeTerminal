#include "doctest.h"

#include "engine/broker.h"
#include "engine/engine.h"
#include "engine/events.h"
#include "engine/exec_sim.h"
#include "engine/portfolio.h"
#include "tt/strategy_api.h"
#include "tt/strategy_registry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

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
// whatever comes back. The base-class default and the whole no-blank-reason
// invariant now live in engine/tests/test_reject.cpp; this stub stays because
// several cases below need a broker that does nothing.
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

// ---- order lifecycle: a death that nobody reports -------------------------
// Strategies gate new entries on "an order is in flight" and only ever clear
// the id in on_fill. A rejected order never reaches on_fill, so before SDK v4
// one reject silenced that symbol for the whole session with nothing in the log
// to say why.
namespace {
// Accepts every submit with a fresh id, then asynchronously rejects it — the
// production shape (IBKR acks the order, then reports it Inactive).
struct RejectingBroker : IBrokerAdapter {
    std::mutex mu;
    std::deque<EngineEvent> q;
    uint64_t next = 1;
    uint64_t submit(const OrderRequest& r, int64_t) override {
        std::lock_guard l(mu);
        const uint64_t id = next++;
        EngineEvent e{};
        e.type = static_cast<uint16_t>(EvType::OrderCancel);
        e.flags = kEvFlagRejected;
        e.symbol_id = r.symbol_id;
        e.u.order.order_id = id;
        q.push_back(e);
        return id;
    }
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
};

// The in-flight guard every shipped strategy uses. `handles_end == false` is
// the pre-v4 strategy: it never learns the id died.
struct GatedEntryStrat : IStrategy {
    bool handles_end;
    std::atomic<int> attempts{0};
    std::atomic<int> ends{0};
    uint64_t entry_id = 0;   // engine thread only
    explicit GatedEntryStrat(bool h) : handles_end(h) {}
    void on_init(IStrategyContext&) noexcept override { entry_id = 0; }
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        if (entry_id != 0) return;   // "an order is in flight, wait"
        entry_id = ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 10,
                                     0, 0, 0, 0});
        if (entry_id) ++attempts;
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd& e) noexcept override {
        ++ends;
        if (handles_end && e.order_id == entry_id) entry_id = 0;
    }
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

} // namespace

// ---- the simulator has no cancel event -------------------------------------
// A real broker reports every cancel. ExecSim just erased orders, so in
// backtests and paper-live an OCO sibling vanished with nothing reported — the
// same wedge, in the path the optimizer grades every candidate on.
TEST_CASE("exec sim: the OCO sibling dropped when a bracket leg fills is reported") {
    ExecParams p;
    p.latency_ns = 0;
    p.latency_jitter_ns = 0;
    p.slippage_bps = 0;
    ExecSim sim(p);
    std::vector<Fill> fills;
    // Bracketed buy: take-profit 110, stop-loss 90.
    const uint64_t parent =
        sim.submit({1, Side::Buy, OrdType::Market, {}, 10, 0, 0, 110.0, 90.0}, 0);
    CHECK(parent != 0);
    sim.on_price(1, 100.0, 1, fills);   // parent fills; TP + SL children spawn
    CHECK(sim.take_cancels().empty());  // nothing has died yet
    CHECK(sim.open_orders() == 2);

    fills.clear();
    sim.on_price(1, 111.0, 2, fills);   // TP fills -> the stop must be reported
    REQUIRE(fills.size() == 1);
    const auto cancels = sim.take_cancels();
    REQUIRE(cancels.size() == 1);
    CHECK(cancels[0].symbol_id == 1);
    CHECK(cancels[0].order_id != fills[0].order_id);
    CHECK(sim.open_orders() == 0);
}

TEST_CASE("exec sim: an explicit cancel reports itself and its OCO partner") {
    ExecParams p;
    p.latency_ns = 0;
    p.latency_jitter_ns = 0;
    p.slippage_bps = 0;
    ExecSim sim(p);
    std::vector<Fill> fills;
    sim.submit({1, Side::Buy, OrdType::Market, {}, 10, 0, 0, 110.0, 90.0}, 0);
    sim.on_price(1, 100.0, 1, fills);
    sim.take_cancels();
    REQUIRE(sim.open_orders() == 2);

    // Cancel one leg: the group goes, and BOTH ids must be reported — the
    // partner is exactly the id a strategy would otherwise wait on forever.
    fills.clear();
    sim.on_price(1, 100.0, 2, fills);   // no fill, just to keep ids stable
    uint64_t leg = 0;
    for (uint64_t id = 1; id < 8 && !leg; ++id)
        if (id != 1 && sim.cancel(id)) leg = id;
    REQUIRE(leg != 0);
    const auto cancels = sim.take_cancels();
    CHECK(cancels.size() == 2);
    CHECK(sim.open_orders() == 0);
}

TEST_CASE("broker: default take_reject reports no BROKER CODE, but does explain") {
    // Was "reports no reason", and asserted r.message.empty() - the base class
    // was pinning the 2026-08-13 defect in place. The numeric code is still 0
    // (there is no broker number to report), but the sentence is not blank.
    // engine/tests/test_reject.cpp owns the rest of this contract.
    StubBroker b;
    const RejectReason r = b.take_reject(42);
    CHECK(r.code == 0);
    CHECK(r.cause == RejectCause::BrokerRefused);
    CHECK_FALSE(r.message.empty());
}

// ---- hot-restart reconciliation: gate dispatch, hold adopted positions until
// flat, then resume. Drives run_live with a scripted broker + injected ticks.
namespace {
struct FakeReconcileBroker : IBrokerAdapter {
    std::mutex mu;
    std::deque<EngineEvent> q;   // events the engine will drain via poll_event
    std::atomic<int> cancel_all_calls{0};
    std::vector<uint64_t> cancelled;   // order ids passed to cancel() (under mu)

    void emit(const EngineEvent& e) {
        std::lock_guard l(mu);
        q.push_back(e);
    }
    uint64_t submit(const OrderRequest&, int64_t) override { return 1; }
    bool cancel(uint64_t id) override {
        std::lock_guard l(mu);
        cancelled.push_back(id);
        return true;
    }
    void cancel_all() override { ++cancel_all_calls; }
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
// Drives a GatedEntryStrat against a broker that rejects everything and returns
// how many entries it managed before going quiet.
int entries_against_rejects(bool handles_end) {
    Engine eng;
    RejectingBroker broker;
    GatedEntryStrat strat(handles_end);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;   // no bars fire; drive the on_tick path
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump_until(eng, [&] { return strat.attempts.load() >= 5; }, 1500);
    const int n = strat.attempts.load();
    eng.stop_live();
    return n;
}
} // namespace

TEST_CASE("order lifecycle: an unreported reject wedges the symbol for the session") {
    // The defect, reproduced: one order goes out, the broker rejects it, and the
    // strategy waits forever on an id that will never reach on_fill. This is
    // what "the strategy just stopped trading" looked like.
    CHECK(entries_against_rejects(/*handles_end=*/false) == 1);
}

TEST_CASE("order lifecycle: on_order_end lets a strategy recover from a reject") {
    CHECK(entries_against_rejects(/*handles_end=*/true) >= 5);
}

TEST_CASE("order lifecycle: a broker reject reaches the strategy") {
    Engine eng;
    RejectingBroker broker;
    GatedEntryStrat strat(true);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    CHECK(pump_until(eng, [&] { return strat.ends.load() >= 1; }));
    eng.stop_live();
}

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

// stop_live(keep_broker_orders=true) must NOT cancel resting broker orders — the
// keep-positions restart re-adopts them; cancelling would leave the position
// naked + paused. Default stop cancels them (nothing should outlive the session).
TEST_CASE("live stop: keep_broker_orders leaves resting orders for re-adoption") {
    Engine eng;
    FakeReconcileBroker broker;
    RecordingStrat s;
    broker.emit(ev_acct(100'000.0));
    broker.emit(ev_reconcile_end());   // flat; reconciliation completes at once
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.broker = &broker;
    cfg.bar_seconds = 100'000;
    REQUIRE(eng.start_live(cfg, {&s}));
    REQUIRE(pump_until(eng, [&] { return eng.live_snapshot().reconciled; }));
    CHECK(broker.cancel_all_calls.load() == 0);
    eng.stop_live(/*keep_broker_orders=*/true);
    CHECK(broker.cancel_all_calls.load() == 0);   // kept, not cancelled
}

TEST_CASE("live stop: default cancels resting broker orders on stop") {
    Engine eng;
    FakeReconcileBroker broker;
    RecordingStrat s;
    broker.emit(ev_acct(100'000.0));
    broker.emit(ev_reconcile_end());
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.broker = &broker;
    cfg.bar_seconds = 100'000;
    REQUIRE(eng.start_live(cfg, {&s}));
    REQUIRE(pump_until(eng, [&] { return eng.live_snapshot().reconciled; }));
    eng.stop_live();   // default keep_broker_orders=false
    CHECK(broker.cancel_all_calls.load() >= 1);
}

// A position snapshot that arrives AFTER reconciliation has ended must be
// ignored, not applied — otherwise a slow-connect failsafe (or a stray late
// replay) clobbers a position the strategy has since traded and re-pauses it.
TEST_CASE("reconciliation: a late position snapshot is ignored, not adopted") {
    Engine eng;
    FakeReconcileBroker broker;
    RecordingStrat s;
    broker.emit(ev_acct(100'000.0));
    broker.emit(ev_reconcile_end());   // flat; reconciliation ends immediately
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.broker = &broker;
    cfg.bar_seconds = 100'000;
    REQUIRE(eng.start_live(cfg, {&s}));
    REQUIRE(pump_until(eng, [&] { return eng.live_snapshot().reconciled; }));
    REQUIRE(eng.live_snapshot().symbols[0].position.qty == doctest::Approx(0.0));

    // Stray late PosSnap: must NOT be adopted (reconciliation is over).
    broker.emit(ev_pos(1, 500.0, 50.0));
    for (int i = 0; i < 40; ++i) eng.push_live_tick("AAA", 1, 50.0, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(eng.live_snapshot().symbols[0].position.qty == doctest::Approx(0.0));
    eng.stop_live();
}

// When a protective stop is rejected, the naked-position safety net flattens +
// halts that symbol. The halted strategy can no longer manage its manual OCO,
// so the flatten MUST also cancel the still-resting sibling leg (the take-
// profit) — otherwise it fills later and reverses the flat book into a naked
// short.
TEST_CASE("protective-reject flatten cancels the orphaned sibling exit leg") {
    Engine eng;
    FakeReconcileBroker broker;
    RecordingStrat s;
    // Adopt AAA long 100 with two resting exits: protective stop #5001, TP #5002.
    broker.emit(ev_pos(1, 100.0, 50.0));
    broker.emit(ev_order(1, 5001, 100.0, Side::Sell, OrdType::Stop, 45.0));
    broker.emit(ev_order(1, 5002, 100.0, Side::Sell, OrdType::Limit, 55.0));
    broker.emit(ev_acct(100'000.0));
    broker.emit(ev_reconcile_end());
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.broker = &broker;
    cfg.bar_seconds = 100'000;
    REQUIRE(eng.start_live(cfg, {&s}));
    REQUIRE(pump_until(eng, [&] {
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0) &&
               eng.live_snapshot().reconciled;
    }));

    // The broker rejects the protective stop (#5001) with the protective flag.
    EngineEvent rej{};
    rej.type = static_cast<uint16_t>(EvType::OrderCancel);
    rej.flags = kEvFlagRejected | kEvFlagProtective;
    rej.symbol_id = 1;
    rej.u.order.order_id = 5001;
    broker.emit(rej);

    // The still-Working TP (#5002) must be cancelled by the flatten.
    REQUIRE(pump_until(eng, [&] {
        std::lock_guard l(broker.mu);
        return std::find(broker.cancelled.begin(), broker.cancelled.end(), 5002u) !=
               broker.cancelled.end();
    }));
    eng.stop_live();
}

namespace {
// Submits one oversized market buy on its second tick (so the engine already
// has a last price to size the notional cap against), then stays quiet.
struct OversizedBuyStrat : IStrategy {
    double qty;
    int seen = 0;
    bool sent = false;
    explicit OversizedBuyStrat(double q) : qty(q) {}
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        if (sent || ++seen < 2) return;
        ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, qty, 0, 0, 0, 0});
        sent = true;
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

namespace {
// Buys 100 on its second tick, then on the fill arms a protective stop below
// entry (a losing exit) and a take-profit above (a winning exit).
struct BracketStrat : IStrategy {
    uint32_t sym = 0;
    int seen = 0;
    bool sent = false;
    bool armed = false;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        sym = sid;
        if (sent || ++seen < 2) return;
        ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0});
        sent = true;
    }
    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        if (f.side == Side::Buy && !armed) {
            armed = true;
            ctx.submit_order({sym, Side::Sell, OrdType::Stop, {}, 100, 0, 45.0, 0, 0});
            ctx.submit_order({sym, Side::Sell, OrdType::Limit, {}, 100, 55.0, 0, 0, 0});
        }
    }
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

void push_n(Engine& eng, const char* sym, double px, int n) {
    for (int i = 0; i < n; ++i) eng.push_live_tick(sym, 1, px, 0.0);
}
} // namespace

TEST_CASE("live risk: hold mode refuses a losing exit but allows a winning one") {
    Engine eng;
    BracketStrat strat;

    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.risk.max_order_qty = 100'000;
    cfg.risk.max_position_qty = 100'000;
    cfg.risk.disable_auto_halt = true;   // hold — don't sell at a loss
    REQUIRE(eng.start_live(cfg, {&strat}));

    // Enter long 100 @ 50 (avg 50); on the fill the strategy arms stop@45 + tp@55.
    REQUIRE(pump_until(eng, [&] {
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0);
    }));

    // Drive price down THROUGH the 45 stop: the protective stop was refused
    // (losing exit), so the position is held, not stopped out.
    for (int i = 0; i < 200; ++i) {
        push_n(eng, "AAA", 44.0, 5);
        if (eng.live_snapshot().symbols[0].position.qty != doctest::Approx(100.0)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0));

    // Now up through the 55 take-profit: a winning exit is allowed, so it closes.
    REQUIRE(pump_until(eng, [&] {
        push_n(eng, "AAA", 56.0, 3);
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(0.0);
    }));

    eng.stop_live();
}

TEST_CASE("live risk: notional cap down-sizes an oversized entry to fit the budget") {
    Engine eng;
    OversizedBuyStrat strat(1'000.0);   // wants 1000 sh; the cap allows far fewer

    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;               // no bars fire; assert on on_tick path
    cfg.risk.max_order_qty = 100'000;        // keep the share caps out of the way
    cfg.risk.max_position_qty = 100'000;
    cfg.risk.max_position_notional = 5'000;  // @ price 50 -> 100 shares
    REQUIRE(eng.start_live(cfg, {&strat}));

    // pump_until feeds AAA @ 50, so the 1000-share buy is clamped to 5000/50 = 100.
    REQUIRE(pump_until(eng, [&] {
        return eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0);
    }));
    // And it holds there — a notional-capped position never overshoots or re-adds.
    for (int i = 0; i < 30; ++i) eng.push_live_tick("AAA", 1, 50.0, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0));

    eng.stop_live();
}

namespace {
// Sizes one entry as alloc_pct% of ctx.budget() — the sizing pattern every
// shipped strategy uses. Records the budget it saw so the test can assert on
// the number, not just on the resulting position.
struct BudgetSizedStrat : IStrategy {
    double alloc_pct;
    int seen = 0;
    bool sent = false;
    std::atomic<double> saw_budget{-1};
    explicit BudgetSizedStrat(double pct) : alloc_pct(pct) {}
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick& t) noexcept override {
        if (sent || ++seen < 2) return;
        const double b = ctx.budget(sid);
        saw_budget = b;
        const double qty = std::floor(b * (alloc_pct / 100.0) / t.price);
        if (qty >= 1.0)
            ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, qty, 0, 0, 0, 0});
        sent = true;
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// Runs one BudgetSizedStrat against a notional cap and returns the position it
// ended up with (0 if it never filled).
double run_sized(double alloc_pct, double cap, double cash = 1'000'000.0) {
    Engine eng;
    BudgetSizedStrat strat(alloc_pct);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;   // no bars fire; assert on the on_tick path
    cfg.initial_cash = cash;
    cfg.risk.max_order_qty = 100'000;
    cfg.risk.max_position_qty = 100'000;
    cfg.risk.max_position_notional = cap;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty > 0.0; });
    const double qty = eng.live_snapshot().symbols[0].position.qty;
    eng.stop_live();
    return qty;
}
} // namespace

// The defect this pins: strategies sized off cash() and the engine then clamped
// the order to the notional cap, so every allocation above the cap collapsed to
// the same position and the knob did nothing. budget() hands them the cap up
// front, so the percentage survives to the fill.
TEST_CASE("sizing: alloc_pct off budget() actually moves the position size") {
    // pump_until feeds AAA @ 50 and the cap is 5000 -> 100 shares is the ceiling.
    CHECK(run_sized(100.0, 5'000.0) == doctest::Approx(100.0));
    CHECK(run_sized(50.0, 5'000.0) == doctest::Approx(50.0));
    CHECK(run_sized(25.0, 5'000.0) == doctest::Approx(25.0));
}

TEST_CASE("sizing: the same allocations off cash() would all collapse to the cap") {
    // The pre-fix behaviour, reproduced through the engine's clamp: 100%, 50%
    // and 25% of a $1M account are all far past a $5k cap, so all three arrive
    // as the identical 100-share position. This is what made alloc_pct inert.
    Engine eng;
    for (double pct : {100.0, 50.0, 25.0}) {
        OversizedBuyStrat strat(std::floor(1'000'000.0 * (pct / 100.0) / 50.0));
        LiveConfig cfg;
        cfg.symbols = {"AAA"};
        cfg.bar_seconds = 100'000;
        cfg.initial_cash = 1'000'000.0;
        cfg.risk.max_order_qty = 100'000;
        cfg.risk.max_position_qty = 100'000;
        cfg.risk.max_position_notional = 5'000;
        REQUIRE(eng.start_live(cfg, {&strat}));
        pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty > 0.0; });
        CHECK(eng.live_snapshot().symbols[0].position.qty == doctest::Approx(100.0));
        eng.stop_live();
    }
}

TEST_CASE("sizing: budget() falls back to cash when no notional cap is set") {
    // A plain backtest configures no cap, so the budget is the account less the
    // fee/slippage reserve: 100% of $10,000 * 0.95 / $50 = 190 shares.
    CHECK(run_sized(100.0, 0.0, 10'000.0) == doctest::Approx(190.0));
}

TEST_CASE("sizing: budget() is bounded by cash even when the cap exceeds it") {
    // Cap far above the account: cash, not the cap, has to bind — otherwise the
    // order comes back rejected for buying power instead of down-sized.
    CHECK(run_sized(100.0, 500'000.0, 10'000.0) == doctest::Approx(190.0));
}

namespace {
// Reports what the engine answered for risk_budget(), from inside a callback.
struct RiskBudgetProbe : IStrategy {
    std::atomic<double> saw{-1.0};
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        saw = ctx.risk_budget(sid);
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

double probe_risk_budget(double per_trade_risk, double notional_cap) {
    Engine eng;
    RiskBudgetProbe strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 1'000'000.0;
    cfg.risk.max_position_notional = notional_cap;
    cfg.risk.per_trade_risk = per_trade_risk;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump_until(eng, [&] { return strat.saw.load() >= 0.0; });
    const double got = strat.saw.load();
    eng.stop_live();
    return got;
}
}  // namespace

// EXPOSURE and RISK are two different ceilings and the engine hands out both.
// Nothing exercised this accessor when it was added: every sizing test drives a
// strategy through a fake context, so a risk_budget() that returned the notional
// cap instead passed the entire suite. The numbers below are deliberately
// unequal so that confusion cannot hide.
TEST_CASE("sizing: risk_budget() is the loss allowance, not the notional cap") {
    CHECK(probe_risk_budget(500.0, 5'000.0) == doctest::Approx(500.0));
}

TEST_CASE("sizing: an unset risk budget is 0 even when a notional cap exists") {
    // 0 is the plain-backtest case and strategies read it as "no ceiling from
    // here". Falling back to the notional cap would silently hand them an
    // exposure figure to divide by a stop distance — a number 4x too large here,
    // and wrong by an unbounded factor in general.
    CHECK(probe_risk_budget(0.0, 5'000.0) == doctest::Approx(0.0));
}

TEST_CASE("sizing: the risk budget is not clamped by cash the way budget() is") {
    // budget() is what you may SPEND, so cash bounds it. risk_budget() is what
    // you may LOSE; the two have no common ceiling and clamping this one against
    // an account balance would be a category error.
    CHECK(probe_risk_budget(2'000'000.0, 5'000.0) == doctest::Approx(2'000'000.0));
}

// ---- the snapshot reports the params that are actually running -------------
// /diag used to read the terminal's Trade-tab copy, which the optimizer
// overwrites with every crowned champion whether or not the live engine took
// it. That made /diag report parameter sets that were never trading — and it
// misled a real diagnosis. The engine's own view is the only ground truth.
TEST_CASE("live snapshot: per-symbol params reflect the engine, and follow a swap") {
    Engine eng;
    RecordingStrat strat;

    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.symbol_params = {{{"alpha", 1.0}, {"beta", 2.0}}};
    REQUIRE(eng.start_live(cfg, {&strat}));
    REQUIRE(pump_until(eng, [&] {
        return !eng.live_snapshot().symbols[0].params.empty();
    }));
    {
        const auto p = eng.live_snapshot().symbols[0].params;
        CHECK(p.at("alpha") == doctest::Approx(1.0));
        CHECK(p.at("beta") == doctest::Approx(2.0));
    }

    // A params update must show up as the new live set, not the old one.
    eng.update_symbol_params(1, {{"alpha", 9.0}, {"beta", 8.0}});
    CHECK(pump_until(eng, [&] {
        const auto p = eng.live_snapshot().symbols[0].params;
        const auto it = p.find("alpha");
        return it != p.end() && it->second == 9.0;
    }));
    CHECK(eng.live_snapshot().symbols[0].params.at("beta") == doctest::Approx(8.0));

    eng.stop_live();
}

// ---- live warmup replay ----------------------------------------------------
// Live sessions only ever get bars from tick aggregation, so a strategy whose
// lookback exceeds one session's worth of bars could never warm up. LiveConfig
// carries seed bars that are replayed straight after on_init.
namespace {
struct WarmupStrat : IStrategy {
    std::atomic<int> inits{0};
    std::atomic<int> bars{0};
    std::atomic<int> accepted{0};       // submit_order returned a real order id
    std::atomic<double> last_close{0};  // engine thread writes, test thread reads
    std::atomic<double> alpha{-1};      // param as seen at the last on_init
    void on_init(IStrategyContext& ctx) noexcept override {
        bars = 0;
        alpha = ctx.param("alpha", -1.0);
        ++inits;
    }
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar& b) noexcept override {
        last_close = b.close;
        ++bars;
        // Every bar tries to trade. During the replay this must be refused.
        OrderRequest r{};
        r.symbol_id = sid;
        r.side = Side::Buy;
        r.type = OrdType::Market;
        r.qty = 1.0;
        if (ctx.submit_order(r) != 0) ++accepted;
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

std::vector<Bar> seed(int n, double base) {
    std::vector<Bar> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = base + i;
        v.push_back(Bar{static_cast<int64_t>(i) * 300'000'000'000LL, p, p, p, p, 10.0});
    }
    return v;
}

// The replay runs on the live thread inside run_live's init, so start_live()
// returns before it has finished. Never REQUIRE before stop_live() in these
// tests: an aborted test case skips stop_live and the engine thread then walks
// into the destroyed stack strategy.
template <class Pred>
bool wait_for(Pred pred, int ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return pred();
}
} // namespace

TEST_CASE("live warmup: seed bars reach the strategy before any tick") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    cfg.symbol_warmup = {seed(250, 100.0)};
    CHECK(eng.start_live(cfg, {&s}));

    // No tick is ever pushed: the only bars this strategy can see are seeds.
    const bool warmed = wait_for([&] { return s.bars.load() >= 250; });
    CHECK(warmed);
    CHECK(s.inits.load() == 1);
    CHECK(s.bars.load() == 250);
    CHECK(s.last_close.load() == doctest::Approx(349.0));   // 100 + 249
    // ...and not one of those 250 bars was allowed to place an order.
    CHECK(s.accepted.load() == 0);
    eng.stop_live();
}

TEST_CASE("live warmup: replayed bars place no orders but live bars do") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 1;             // let real bars close quickly
    cfg.symbol_warmup = {seed(40, 10.0)};
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.bars.load() >= 40; }));
    CHECK(s.accepted.load() == 0);

    // Now drive real ticks across a bar boundary: the same code path that was
    // muted during the replay must work normally afterwards. The tick timestamp
    // has to advance -- roll_bar only closes a bar when one crosses the edge.
    int64_t ts_ms = 1'000'000;
    const bool traded = wait_for([&] {
        ts_ms += 1500;   // > bar_seconds, so every other tick closes a bar
        eng.push_live_tick("AAA", ts_ms, 50.0, 0.0);
        return s.accepted.load() > 0;
    });
    CHECK(traded);
    CHECK(s.bars.load() > 40);
    eng.stop_live();
}

TEST_CASE("live warmup: a params swap re-seeds instead of restarting cold") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    cfg.symbol_warmup = {seed(30, 100.0)};
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.bars.load() >= 30; }));

    // A params-only update re-inits the strategy (wiping its history), so it
    // must be handed fresh bars or it would restart from zero.
    eng.update_symbol_params(1, {{"foo", 1.0}}, seed(120, 5.0));
    const bool reinit = wait_for([&] {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);   // swaps apply on the live loop
        return s.inits.load() >= 2 && s.bars.load() >= 120;
    });
    CHECK(reinit);
    CHECK(s.inits.load() == 2);
    CHECK(s.bars.load() >= 120);     // re-init zeroed the counter, replay refilled it
    CHECK(s.accepted.load() == 0);   // still nothing placed from history
    eng.stop_live();
}

TEST_CASE("live warmup: reseed_symbol warms a cold symbol without touching params") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    cfg.symbol_params = {{{"alpha", 7.0}}};
    // Started cold: the candle cache is memory-only, so the first session after
    // launch has nothing to seed from.
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.inits.load() >= 1; }));
    CHECK(s.bars.load() == 0);

    // History arrives later; re-seed must warm the strategy and leave the
    // symbol's params exactly as they were.
    eng.reseed_symbol(1, seed(200, 20.0));
    const bool warmed = wait_for([&] {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);   // swaps apply on the live loop
        return s.bars.load() >= 200;
    });
    CHECK(warmed);
    CHECK(s.inits.load() == 2);       // re-init, then replay
    CHECK(s.accepted.load() == 0);    // replayed bars still place nothing
    CHECK(s.alpha.load() == doctest::Approx(7.0));   // params survived the re-seed
    eng.stop_live();
}

TEST_CASE("live warmup: reseed_symbol with no bars is a no-op") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.inits.load() >= 1; }));

    // An empty fetch must not trigger a pointless re-init (which would WIPE the
    // history the symbol had already accumulated from live bars).
    eng.reseed_symbol(1, {});
    for (int i = 0; i < 20; ++i) {
        eng.push_live_tick("AAA", 1000 + i * 400, 50.0, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(s.inits.load() == 1);
    eng.stop_live();
}

// ---- log origin tagging -----------------------------------------------------
// Backtests and the live loop share one log queue. The UI has to file backtest
// floods into the optimizer panel and live output into the console, and it used
// to decide with a global "is anything optimizing?" test — which is true for
// most of a trading day once a 30-minute autopilot is running, so live fills and
// strategy lines were being buried in the optimizer log.
TEST_CASE("log lines are tagged with the thread they came from") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    cfg.symbol_warmup = {seed(5, 10.0)};
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.bars.load() >= 5; }));

    // Everything the live session emitted must be marked live.
    std::string line;
    bool from_live = false;
    int live_lines = 0, other = 0;
    bool saw_started = false;
    while (eng.pop_log(line, from_live)) {
        if (from_live) ++live_lines; else ++other;
        if (line.find("BROKER trading") != std::string::npos ||
            line.find("paper trading") != std::string::npos) {
            saw_started = true;
            CHECK(from_live);          // the session banner is live output
        }
        if (line.find("warmed on") != std::string::npos) CHECK(from_live);
    }
    CHECK(saw_started);
    CHECK(live_lines > 0);
    CHECK(other == 0);                 // nothing here came from a backtest
    eng.stop_live();
}

TEST_CASE("the legacy single-argument pop_log still drains") {
    Engine eng;
    WarmupStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.inits.load() >= 1; }));
    std::string line;
    int n = 0;
    while (eng.pop_log(line)) ++n;
    CHECK(n > 0);
    eng.stop_live();
}

// ---------------------------------------------------------------------------
// THE DAY BOUNDARY THE BACKTEST DID NOT HAVE (BacktestConfig::eod_flatten_h).
//
// 2026-08-14, -$506.26 on STKH. The morning tournament fitted its
// bollinger_reversion time_stop to 233 bars and scored it +8.60% — a 19.4-hour
// hold on a 300 s series. That score was reachable because Engine::run replayed
// six months of bars straight through with no session concept at all. Live, the
// engine force-flattens everything at 15:57, so 77 bars is the ceiling; and
// BollRev places no price stop by design, which makes that time stop the ONLY
// exit a losing position has. It could not fire from the instant it was crowned.
// The position filled at 13:41, held 27 bars, fell 10.1%, and was liquidated by
// the backstop.
//
// The optimizer was fitting under physics production does not have. Clamping the
// parameter afterwards is a guard rail; this is the source fix, and without it
// every time-based parameter of every strategy keeps inheriting the same lie.
namespace {

// Buys once, on its second bar, and then does nothing at all. Deliberately has
// NO exit: the point is whether the ENGINE closes the day, so a strategy that
// could close itself would prove nothing. This is bollinger_reversion's shape on
// a losing trade, reduced to its essentials.
struct BuyAndNeverExit final : IStrategy {
    int bars = 0;
    int fills = 0;
    int order_ends = 0;
    bool bought = false;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        if (++bars != 2 || bought) return;
        bought = true;
        ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0});
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override { ++fills; }
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {
        ++order_ends;
    }
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// Bars on the LOCAL clock, built through mktime so the case means the same thing
// in every timezone — hour_of_day_local reads the machine's zone, and a
// hardcoded epoch would test a different hour of the day on every box.
// `days` sessions of 5-minute bars, from the 09:30 open through a 16:00 open.
//
// A bar COMPLETES at open + interval (engine.cpp), and the boundary is tested
// against the completion, so bar i is stamped 09:35 + i*5m: i=76 is 15:55, i=77
// is 16:00 — the one that crosses 15:57 — and i=78 is 16:05.
//
// last_open_i extends the session into after-hours. It matters: with the default
// 78 there are only two bars past the cutoff and the strategy below re-enters at
// most once, so an edge trigger and a LEVEL trigger produce identical fills and
// the crossing test cannot be distinguished from a threshold test. 126 runs to a
// 20:00 open, which is the shape the live feed actually delivers.
std::vector<Bar> local_session_bars(int days, int last_open_i = 78) {
    std::vector<Bar> out;
    for (int d = 0; d < days; ++d) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900;
        tm.tm_mon = 8 - 1;
        tm.tm_mday = 17 + d;    // Mon 2026-08-17 onward: consecutive weekdays
        tm.tm_hour = 9;
        tm.tm_min = 30;
        tm.tm_isdst = -1;
        const std::time_t open = std::mktime(&tm);
        REQUIRE(open != static_cast<std::time_t>(-1));
        for (int i = 0; i <= last_open_i; ++i) {   // 09:30 .. 16:00 opens
            Bar b{};
            b.ts_ns = static_cast<int64_t>(open + i * 300) * 1000000000LL;
            b.open = b.close = 10.0;
            b.high = 10.05;
            b.low = 9.95;
            b.volume = 1e6;
            out.push_back(b);
        }
    }
    return out;
}
} // namespace

TEST_CASE("backtest: without a day boundary a position rides forever") {
    // The behaviour that scored STKH's 233-bar hold at +8.60%. Kept as a case in
    // its own right because it is still the CORRECT behaviour for a manual
    // backtest and for a replay, whose clock is the data's and not the world's —
    // eod_flatten_h defaults to 0 for exactly that reason, and a change that
    // switched it on everywhere would silently rewrite every saved replay.
    Engine eng;
    BuyAndNeverExit strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(2);
    cfg.synth_ticks = false;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    REQUIRE(r.fills.size() == 1);                     // the entry, and nothing else
    CHECK(r.fills[0].side == static_cast<uint8_t>(Side::Buy));
}

TEST_CASE("backtest: eod_flatten_h closes the position the live engine would") {
    Engine eng;
    BuyAndNeverExit strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(2);
    cfg.synth_ticks = false;
    cfg.eod_flatten_h = 15.95;   // 15:57, mirroring run_live's kEodBackstopH
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);

    // The entry, then the liquidation the live engine performs every afternoon.
    // Without it this run books ONE fill and carries the position across the day
    // boundary — which is how a 233-bar time stop came to look realizable.
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].side == static_cast<uint8_t>(Side::Buy));
    CHECK(r.fills[1].side == static_cast<uint8_t>(Side::Sell));
    CHECK(r.fills[1].qty == doctest::Approx(100));
    // The strategy is TOLD, so its own bookkeeping stays honest — every strategy
    // here latches an entry_id_/exit_id_ in-flight guard, and a liquidation it
    // never heard about would wedge the symbol for the rest of the run.
    CHECK(strat.fills == 2);
    // Through ExecSim, so the close pays the same slippage and the same IBKR
    // commission as any other fill. A free liquidation would flatter exactly the
    // fits this exists to penalise.
    CHECK(r.fills[1].fee > 0.0);
}

TEST_CASE("backtest: the boundary is compared against the LOCAL clock") {
    // A cutoff above every hour in the data must close nothing at all — the
    // sanity check that the crossing test is reading an hour of day rather than
    // an epoch, which would fire on the first bar and pass the case above for
    // entirely the wrong reason.
    Engine eng;
    BuyAndNeverExit strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.synth_ticks = false;
    cfg.eod_flatten_h = 23.99;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(r.fills.size() == 1);
}

namespace {

// Re-enters the moment it finds itself flat, which is what makes the EDGE
// trigger observable: the three cases above buy once and never again, so the
// position is already closed for every bar after the crossing and a level test
// passes them all by falling into `if (pos == 0.0) return`.
struct RebuysWhenFlat final : IStrategy {
    int bars = 0;
    double pos = 0.0;
    bool in_flight = false;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        if (++bars < 2 || pos != 0.0 || in_flight) return;
        in_flight = true;
        ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0});
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill& f) noexcept override {
        in_flight = false;
        pos += f.side == Side::Buy ? f.qty : -f.qty;
    }
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// Holds a position AND leaves a resting exit sitting in the book across the
// boundary. The limit is far above every price in the fixture, so nothing but
// the flatten's own cancel_all can ever retire it.
struct RestsAnExitOrder final : IStrategy {
    int bars = 0;
    int order_ends = 0;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        ++bars;
        if (bars == 2)
            ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0});
        else if (bars == 4)
            ctx.submit_order({sid, Side::Sell, OrdType::Limit, {}, 100, 999.0, 0, 0, 0});
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {
        ++order_ends;
    }
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

} // namespace

TEST_CASE("backtest: the boundary is a CROSSING, not a threshold") {
    // The distinction the three cases above cannot make, and the one the live
    // backstop documents at engine.cpp ("a level trigger would flatten the
    // instant the engine came up at, say, 16:10"). v0.25.1 shipped exactly this
    // bug in the session guard, so it is worth a case that fails on it.
    //
    // Two sessions of bars running to 20:05, and a strategy that re-enters
    // whenever it is flat:
    //   EDGE  — flatten at day 1's 16:00 crossing, the re-entry rides the whole
    //           evening untouched, flatten again at day 2's 16:00, re-enter and
    //           finish long. Three buys, two sells.
    //   LEVEL — measured, not guessed: 2 fills, 1 sell, ends flat. Every evening
    //           bar is past 15:57, so the flatten runs again on the bar after the
    //           re-entry is submitted and its cancel_all kills that entry before
    //           it can fill. The strategy is left with its in-flight guard latched
    //           and never trades again — the symbol is wedged for the rest of the
    //           run, which scores as "this fit stops trading after day one".
    Engine eng;
    RebuysWhenFlat strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(2, 126);   // 09:30 .. 20:00 opens
    cfg.synth_ticks = false;
    cfg.eod_flatten_h = 15.95;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);

    CHECK(r.fills.size() == 5);
    int sells = 0;
    for (const TradeRow& f : r.fills)
        if (f.side == static_cast<uint8_t>(Side::Sell)) ++sells;
    CHECK(sells == 2);            // one per day, not one per evening bar
    CHECK(strat.pos == doctest::Approx(100));   // finishes long, never re-closed
}

TEST_CASE("backtest: the flatten retires resting orders and says so") {
    // A liquidation that leaves the strategy's exit order alive is worse than no
    // liquidation: bollinger_reversion (and every other strategy here) latches an
    // entry_id_/exit_id_ in-flight guard and will not trade again until it hears
    // the order end. Deleting the cancel_all loop wedges the symbol for the rest
    // of the run, and the fit scores as "stops trading after day one" rather than
    // as the flat position it should be.
    Engine eng;
    RestsAnExitOrder strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(2);
    cfg.synth_ticks = false;
    cfg.eod_flatten_h = 15.95;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);

    // The entry and the liquidation — NOT the 999.0 exit, which can never fill.
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[1].side == static_cast<uint8_t>(Side::Sell));
    // The resting order died at the boundary and the strategy was told once.
    CHECK(strat.order_ends == 1);
}

// ---------------------------------------------------------------------------
// REALIZED P&L IS GROSS OF COMMISSIONS, AND THE PRODUCT SAID OTHERWISE.
//
// 2026-08-14 closed a reported -$597.01 realized against -$612.09 of actual
// cash: 13 fills, $15.08 of commission, 2.5% of the very number used to judge
// whether the strategies have an edge. Portfolio::apply books
// (price - avg) * qty into realized and charges the fee to cash_ only, so every
// realized-P&L surface understates a loss by exactly the commissions paid.

TEST_CASE("portfolio: realized_net is realized minus the commissions actually paid") {
    Portfolio pf(10000.0);
    const double cash0 = pf.cash();
    pf.apply(mk_fill(1, Side::Buy, 100, 10.0, 1.00));
    pf.apply(mk_fill(1, Side::Sell, 100, 11.0, 1.50));

    // Both numbers, both published. `realized` stays GROSS on purpose: every
    // journal row and every saved session was computed that way, and silently
    // redefining it would stop a P&L series being comparable to itself.
    CHECK(pf.position(1).realized_pnl == doctest::Approx(100.0));
    CHECK(pf.fees(1) == doctest::Approx(2.50));
    CHECK(pf.realized_net(1) == doctest::Approx(97.50));

    // THE INVARIANT nothing asserted before: on a flat round trip, the money the
    // account actually kept IS realized_net. If these two ever disagree, one of
    // them is not describing this account.
    CHECK(pf.position(1).qty == doctest::Approx(0.0));
    CHECK(pf.cash() - cash0 == doctest::Approx(pf.realized_net(1)));
    // The ENTRY's commission counts too. Attributing only the exit's would halve
    // the cost of every completed round trip.
    CHECK(pf.fees(1) > 1.50);
}

TEST_CASE("portfolio: fees are per symbol and reset with the portfolio") {
    Portfolio pf(100000.0);
    pf.apply(mk_fill(1, Side::Buy, 100, 10.0, 1.0));
    pf.apply(mk_fill(2, Side::Buy, 50, 20.0, 2.0));
    CHECK(pf.fees(1) == doctest::Approx(1.0));
    CHECK(pf.fees(2) == doctest::Approx(2.0));
    CHECK(pf.fees() == doctest::Approx(3.0));
    CHECK(pf.fees(99) == doctest::Approx(0.0));   // a symbol that never traded
    CHECK(pf.realized_net(99) == doctest::Approx(0.0));
    // A session's costs belong to that session: reset() clears the slots, so a
    // new session cannot inherit yesterday's commissions into its net P&L.
    pf.reset(100000.0);
    CHECK(pf.fees() == doctest::Approx(0.0));
}

// ---- hold-only symbols -----------------------------------------------------
//
// A lineup swap that drops a symbol while the broker still shows a position
// there must KEEP it in cfg.symbols — a symbol outside the session list can be
// neither adopted, audited nor flattened (2026-08-06 orphan $846; 2026-07-21
// NVDA, undetected for 24 days). But it must not trade.
//
// The first implementation expressed that as strat_key.clear(), which does NOT
// produce a strategy-less tab: App::acquire_strategy maps "" to
// kBuiltinStrategyKey ("sma_crossover.cpp"), a real promoted strategy that then
// runs on its declared defaults and opens a position the moment adopt_hold
// releases the symbol. Hence a first-class flag the ENGINE honours.

namespace {
// Submits one buy, then one sell of the same size, recording what came back.
struct BuyThenSellStrat : IStrategy {
    uint64_t buy_id = 0, sell_id = 0;
    std::atomic<bool> done{false};
    void on_init(IStrategyContext&) noexcept override { buy_id = sell_id = 0; }
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        if (done.load()) return;
        if (buy_id == 0) {
            buy_id = ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 10,
                                       0, 0, 0, 0});
            if (buy_id == 0) done.store(true);   // refused: stop retrying
            return;
        }
        if (ctx.position(sid).qty > 0.0 && sell_id == 0) {
            sell_id = ctx.submit_order({sid, Side::Sell, OrdType::Market, {}, 10,
                                        0, 0, 0, 0});
            done.store(true);
        }
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

TEST_CASE("hold-only: an opening order is refused, and says why") {
    Engine eng;
    BuyThenSellStrat strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.symbol_hold_only = {1};
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump_until(eng, [&] { return strat.done.load(); }, 5000);
    eng.stop_live();
    // Refused: submit_order returns 0 and the record names the cause. A bare
    // rejection here would be the 2026-08-13 defect all over again.
    CHECK(strat.buy_id == 0u);
    const auto snap = eng.live_snapshot();
    REQUIRE(snap.orders.size() >= 1);
    const OrderRecord& r = snap.orders.front();
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_cause == RejectCause::HoldOnlySymbol);
    CHECK_FALSE(r.reject_msg.empty());
    // It never reached a position.
    CHECK(snap.symbols[0].position.qty == doctest::Approx(0.0));
}

TEST_CASE("hold-only: the symbol can still REDUCE, or it would trap the position") {
    // The whole point of carrying the symbol is to let the position OUT. A flag
    // that blocked exits too would turn the orphan-protection into the orphan.
    Engine eng;
    BuyThenSellStrat strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.symbol_hold_only = {};   // opens allowed: build a position first
    REQUIRE(eng.start_live(cfg, {&strat}));
    // Wait for the round trip to COMPLETE, not merely for the sell to be
    // submitted: strat.done is set at submit time, and asserting on the
    // position before the fill lands is a race, not a result.
    CHECK(pump_until(eng, [&] {
        const auto s = eng.live_snapshot();
        return strat.sell_id != 0 && !s.symbols.empty() &&
               s.symbols[0].position.qty == 0.0;
    }, 5000));
    eng.stop_live();
    // Both were accepted, and the pump predicate above is what proves the sell
    // actually REDUCED the position to flat. Re-asserting the quantity after
    // stop_live() would be racing the teardown for no extra information.
    CHECK(strat.buy_id != 0u);
    CHECK(strat.sell_id != 0u);
}

TEST_CASE("hold-only: absent or short vector leaves every symbol tradable") {
    // Parallel-vector hazard: a config that sets it for one symbol and not
    // another must not silently gate the unlisted one.
    Engine eng;
    BuyThenSellStrat strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA", "BBB"};
    cfg.bar_seconds = 100'000;
    cfg.symbol_hold_only = {};   // empty = nothing is hold-only
    REQUIRE(eng.start_live(cfg, {&strat}));
    CHECK(pump_until(eng, [&] { return strat.done.load(); }, 5000));
    eng.stop_live();
    CHECK(strat.buy_id != 0u);
}

// ---------------------------------------------------------------------------
// THE OTHER HALF OF THE DAY (BacktestConfig::entry_cutoff_h).
//
// eod_flatten_h stopped the replay scoring holds production flattens away. It
// does NOT stop the replay scoring ENTRIES production refuses outright: the
// flatten closes a position after the strategy has already opened it, so the
// trade is still booked and still scored. Live, EngineCtx::submit_order refuses
// a position-increasing order past the cutoff with RejectCause::SessionClosed —
// the gate added after 2026-08-13, when two entries went out at 16:00:12 and
// 16:00:17 into "error 201: Exchange is closed" and a third at 16:05 rested
// overnight as a market order for the next open.
namespace {

// Buys on the LAST bar of the session and nowhere else. On the fixture below
// that bar completes at 16:05, well past the 15:57 cutoff — the exact shape of
// the orders IBKR refused.
struct BuysAfterTheClose final : IStrategy {
    int bars = 0;
    int refused = 0;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        if (++bars != 79) return;   // i = 78, the 16:05 completion
        if (ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0}) == 0)
            ++refused;
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

} // namespace

TEST_CASE("backtest: without an entry cutoff the replay books an order live refuses") {
    // The positive control. Still the correct behaviour for a manual backtest and
    // a replay, which is why the field defaults to 0 — but it is a lie in a run
    // whose whole purpose is to score a fit that will be TRADED.
    Engine eng;
    BuysAfterTheClose strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    // TWO sessions, so the 16:05 order has a following bar to fill against. On a
    // one-day fixture it is accepted and then simply never fills, which would
    // make this control pass for the wrong reason — and make the armed case below
    // look like it had worked when it had not.
    cfg.bars = local_session_bars(2);
    cfg.synth_ticks = false;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(strat.refused == 0);      // the engine accepted it
    CHECK(r.fills.size() == 1);     // ...and it filled, and was scored
    // Filled the NEXT MORNING, which is what the live rejects were about: the
    // 16:05 order on 2026-08-13 rested overnight as a market order for the open.
    CHECK(r.fills[0].ts_ns >= local_session_bars(2).back().ts_ns - 86'400LL * 1'000'000'000LL);
}

TEST_CASE("backtest: the entry cutoff refuses it, on the bar's own clock") {
    Engine eng;
    BuysAfterTheClose strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(2);   // same fixture as the control above
    cfg.synth_ticks = false;
    cfg.entry_cutoff_h = 15.95;     // mirrors tt::entry_cutoff_h / kEodBackstopH
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);

    CHECK(strat.refused == 1);      // submit_order returned 0, as it does live
    CHECK(r.fills.empty());         // nothing was booked, so nothing was scored
}

TEST_CASE("backtest: the entry cutoff does not touch entries inside the session") {
    // A cutoff that refuses everything would "fix" the score by silencing the
    // strategy, which is the failure mode to avoid: this must remove exactly the
    // unreachable trades and no others. BuyAndNeverExit buys on its second bar,
    // 09:40, which live would allow.
    Engine eng;
    BuyAndNeverExit strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.synth_ticks = false;
    cfg.entry_cutoff_h = 15.95;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(r.fills.size() == 1);
    CHECK(r.fills[0].side == static_cast<uint8_t>(Side::Buy));
}

// ------------------------------------------------- backtest: order-level risk
//
// The third and last parity hole (BacktestConfig::risk). The two above are about
// WHEN the replay trades; these are about HOW MUCH, which is the one that was
// silently wrong on every score the optimizer ever produced.

namespace {
// Sizes exactly the way every shipped strategy does: off ctx.budget(). That is
// what makes the missing cap a SIZE bug rather than a missing guard rail — with
// no RiskLimits, budget() returns cash * 0.95 and hands over the whole account.
struct SizesOffBudget final : IStrategy {
    int bars = 0;
    bool bought = false;
    double asked = 0;               // qty submitted, before any clamp
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar& b) noexcept override {
        if (++bars != 2 || bought) return;
        bought = true;
        asked = std::floor(ctx.budget(sid) / b.close);
        ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, asked, 0, 0, 0, 0});
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

TEST_CASE("backtest: with no risk, budget() hands the strategy the whole account") {
    // THE MEASURED DEFECT, in miniature and without a strategy module. $100,000
    // of cash, a $10 stock: budget() returns 95,000 and the position is 9,500
    // shares. Live the same symbol is capped at a few thousand dollars.
    //
    // Kept as a case in its own right, and NOT a regression to fix: risk defaults
    // to empty because a manual backtest and a captured replay must keep judging
    // the data's own cash. It is the OPTIMIZER that must arm it.
    Engine eng;
    SizesOffBudget strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.initial_cash = 100'000.0;
    cfg.synth_ticks = false;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(strat.asked == doctest::Approx(9500));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == doctest::Approx(9500));
}

TEST_CASE("backtest: a notional cap sizes the replay the way live is sized") {
    // Same fixture, same strategy, one field set — and the position becomes the
    // 500 shares a $5,000 budget buys. This is the whole fix: the number the
    // strategy is HANDED changes, not just the number it is allowed to keep.
    Engine eng;
    SizesOffBudget strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.initial_cash = 100'000.0;
    cfg.synth_ticks = false;
    RiskLimits rl{};
    rl.max_position_notional = 5'000.0;
    rl.max_order_qty = 10'000;      // out of the way: this case is about budget()
    rl.max_position_qty = 10'000;
    cfg.risk = rl;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(strat.asked == doctest::Approx(500));       // 5,000 / 10.0
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == doctest::Approx(500));
}

TEST_CASE("backtest: max_order_qty REFUSES, it does not silently down-size") {
    // The notional cap clamps; the share caps reject. Both reach the replay now,
    // and they have to stay distinguishable — an optimizer that thought an
    // oversized order was merely trimmed would still score the trade.
    //
    // BuyAndNeverExit asks for a flat 100 shares, so this is independent of
    // budget() and of the cap above.
    Engine eng;
    BuyAndNeverExit strat;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.synth_ticks = false;
    RiskLimits rl{};
    rl.max_order_qty = 50;          // the strategy wants 100
    rl.max_position_notional = 0;   // no clamp: prove the REFUSAL, not a trim
    cfg.risk = rl;
    const BacktestResult r = run_backtest_blocking(eng, cfg, &strat);
    CHECK(r.fills.empty());         // refused outright, nothing booked
    CHECK(r.trades == 0);
}

namespace {
// Pyramids: two 100-share buys on consecutive bars, regardless of position. The
// only shape that can tell max_position_qty (cumulative) from max_order_qty
// (per order) — every other probe here is flat before it buys.
struct AddsTwice final : IStrategy {
    int bars = 0;
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        if (++bars == 2 || bars == 3)
            ctx.submit_order({sid, Side::Buy, OrdType::Market, {}, 100, 0, 0, 0, 0});
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_order_end(IStrategyContext&, const OrderEnd&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

TEST_CASE("backtest: max_position_qty stops the SECOND entry, not the first") {
    // Control first, in the same case, because a cap that refused BOTH orders
    // would satisfy a bare "one fill" assertion just as well as a working one.
    Engine eng;
    AddsTwice loose;
    BacktestConfig cfg;
    cfg.symbol = "STKH";
    cfg.bars = local_session_bars(1);
    cfg.synth_ticks = false;
    const BacktestResult uncapped = run_backtest_blocking(eng, cfg, &loose);
    REQUIRE(uncapped.fills.size() == 2);              // both lots, no risk at all

    Engine eng2;
    AddsTwice strat;
    RiskLimits rl{};
    rl.max_order_qty = 10'000;      // per-order cap out of the way
    rl.max_position_qty = 150;      // one lot fits, two do not
    rl.max_position_notional = 0;
    cfg.risk = rl;
    const BacktestResult r = run_backtest_blocking(eng2, cfg, &strat);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == doctest::Approx(100));
}

// A strategy that rests ONE order at a price the feed will never reach, then
// does nothing. It is flat the whole time, which is exactly the state the swap
// gate used to read as "safe to rebuild".
struct RestingOrderStrat : IStrategy {
    std::atomic<int> inits{0};
    std::atomic<unsigned long long> order{0};
    std::atomic<bool> cancel_now{false};
    void on_init(IStrategyContext& ctx) noexcept override {
        // First init only: a re-init that DOES happen must be visible as
        // inits==2 without a second order confusing what is resting.
        if (inits.fetch_add(1) != 0) return;
        // Buy-stop far above the market: accepted, working, never triggered.
        OrderRequest r{};
        r.symbol_id = 1;
        r.side = Side::Buy;
        r.type = OrdType::Stop;
        r.qty = 10.0;
        r.stop_price = 10'000.0;
        order = ctx.submit_order(r);
    }
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t, const Tick&) noexcept override {
        if (cancel_now.exchange(false) && order.load() != 0)
            ctx.cancel_order(order.load());
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// 2026-08-27, SPCH. orb_breakout armed a 555-share buy-stop at 10:05, the
// autopilot re-seeded the symbol at 10:08, and the stop triggered at 10:35 into
// a strategy instance whose long_stop_id_ had been wiped by on_init. None of
// on_fill's branches matched the fill, so there was no OCO cancel, no protective
// stop and no take-profit: $4,995 of stock with nothing to close it but the
// 15:57 backstop. The swap gate tested only for a flat POSITION, and a strategy
// resting an entry order has no position yet.
TEST_CASE("live swap: a re-init waits for resting orders, not just a flat book") {
    Engine eng;
    RestingOrderStrat s;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 300;
    CHECK(eng.start_live(cfg, {&s}));
    CHECK(wait_for([&] { return s.inits.load() >= 1; }));
    CHECK(s.order.load() != 0);   // the order was accepted and is resting

    // The symbol is FLAT — the old gate's whole test — but not idle.
    eng.update_symbol_params(1, {{"foo", 1.0}});
    for (int i = 0; i < 200; ++i) {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);   // swaps apply on the live loop
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(eng.live_snapshot().symbols[0].position.qty == 0.0);   // flat throughout
    // The point of the case: rebuilding here would wipe the id of an order that
    // is still live at the broker.
    CHECK(s.inits.load() == 1);

    // DEFERRED, NOT DROPPED. Once the order is gone the same queued swap must
    // still land — otherwise this fix would silently stop params updates
    // reaching any strategy that ever rests an order.
    s.cancel_now = true;
    const bool applied = wait_for([&] {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);
        return s.inits.load() >= 2;
    });
    CHECK(applied);
    CHECK(s.inits.load() == 2);
    eng.stop_live();
}

// ---------------------------------------------------------------------------
// A broker-side flatten leaves no order id (0.41.0).
//
// IBrokerAdapter::flatten() returns void and yields nothing the engine can
// record; the adapter's own I/O thread submits the closes under its OWN
// numbering. So on the live route the kill switch and the EOD backstop produced
// fills for ids that `orders` had never heard of — and the loop in apply_fill
// simply matched nothing. The position moved, the P&L booked, and the blotter
// showed an entry with no exit.
//
// 2026-09-08, SNXX: bought 276 @ 18.07, backstop flattened at 17.39 for -$190,
// and the operator saw a buy, a vanished position, realized P&L, and no sell
// order anywhere. The SIM path records properly, which is why nothing in a
// backtest or a replay could ever surface this.
// ---------------------------------------------------------------------------
namespace {
// Models the real adapter: flatten() acknowledges nothing, and the close comes
// back later as a fill under an id the engine never issued.
struct FlatteningBroker : IBrokerAdapter {
    std::mutex mu;
    std::deque<EngineEvent> q;
    std::atomic<int> flatten_calls{0};
    std::atomic<bool> answer_flatten{true};   // false = the flatten never lands
    uint64_t next_id = 1;

    void emit(const EngineEvent& e) {
        std::lock_guard l(mu);
        q.push_back(e);
    }
    uint64_t submit(const OrderRequest&, int64_t) override {
        std::lock_guard l(mu);
        return next_id++;
    }
    bool cancel(uint64_t) override { return true; }
    void cancel_all() override {}
    void flatten() override {
        ++flatten_calls;
        if (!answer_flatten.load()) return;   // requested, never executed
        // Id 9000: emphatically NOT one the engine handed out.
        emit(ev_fill(1, 9000, Side::Sell, 276.0, 17.39));
    }
    bool poll_event(EngineEvent& out) override {
        std::lock_guard l(mu);
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    bool ready() const override { return true; }
};

// Opens one long on its first tick and then does nothing, so the position is
// still there when the kill switch runs.
struct BuyOnceStrat : IStrategy {
    std::atomic<bool> done{false};
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        if (done.exchange(true)) return;
        OrderRequest r{};
        r.symbol_id = sid;
        r.side = Side::Buy;
        r.type = OrdType::Market;
        r.qty = 276.0;
        ctx.submit_order(r);
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// Buy 276 @ 18.07 the engine's way, then kill-switch. Returns the snapshot
// after the flatten has had time to come back (or not).
LiveSnapshot flatten_scenario(bool answer_flatten) {
    Engine eng;
    FlatteningBroker broker;
    broker.answer_flatten = answer_flatten;
    BuyOnceStrat strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;   // no bars; drive on_tick
    cfg.broker = &broker;
    cfg.risk.disable_auto_halt = false;
    eng.start_live(cfg, {&strat});
    // Entry, then its fill under the id the engine DID issue.
    pump_until(eng, [&] { return eng.live_snapshot().orders.size() >= 1; });
    broker.emit(ev_fill(1, 1, Side::Buy, 276.0, 18.07));
    pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty == 276.0; });

    eng.kill_switch();
    pump_until(eng, [&] {
        const LiveSnapshot s = eng.live_snapshot();
        return broker.flatten_calls.load() > 0 &&
               (!answer_flatten || s.symbols[0].position.qty == 0.0);
    });
    // Let a few more publishes go by so a pending row that WOULD clear has.
    pump_until(eng, [] { return false; }, 120);
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    return s;
}
} // namespace

TEST_CASE("flatten: a broker-side close appears in the order book") {
    const LiveSnapshot s = flatten_scenario(/*answer_flatten=*/true);
    CHECK(s.symbols[0].position.qty == 0.0);   // it really did close

    // THE BUG: before 0.41.0 this vector held only the BUY. A fill for an id
    // the engine never submitted matched nothing and was dropped on the floor.
    const OrderRecord* sell = nullptr;
    for (const OrderRecord& o : s.orders)
        if (o.side == static_cast<uint8_t>(Side::Sell)) sell = &o;
    REQUIRE(sell != nullptr);
    CHECK(sell->id == 9000);                       // the BROKER's id, preserved
    CHECK(sell->qty == 276.0);
    CHECK(sell->fill_price == doctest::Approx(17.39));
    CHECK(sell->status == OrderStatus::Filled);
    // Marked, not silently blended in: "the engine placed this" and "something
    // else placed this and we inferred it" are different facts.
    CHECK(sell->broker_originated);

    // The engine's own order is untouched and NOT marked.
    const OrderRecord* buy = nullptr;
    for (const OrderRecord& o : s.orders)
        if (o.side == static_cast<uint8_t>(Side::Buy)) buy = &o;
    REQUIRE(buy != nullptr);
    CHECK_FALSE(buy->broker_originated);
    CHECK(buy->fill_price == doctest::Approx(18.07));
}

TEST_CASE("flatten: a completed flatten leaves nothing outstanding") {
    const LiveSnapshot s = flatten_scenario(/*answer_flatten=*/true);
    CHECK(s.flatten_pending.empty());
}

TEST_CASE("flatten: one that never executes stays outstanding and says so") {
    // The half that matters. A flatten yields no order id, so a request that
    // never reaches the exchange used to be indistinguishable from one that
    // filled — no rows either way. 2026-08-06: the backstop's flatten did not
    // fill, the position sat overnight, $846.
    const LiveSnapshot s = flatten_scenario(/*answer_flatten=*/false);
    CHECK(s.symbols[0].position.qty == 276.0);   // still open, as the bug requires
    REQUIRE(s.flatten_pending.size() == 1);
    CHECK(s.flatten_pending[0].symbol == "AAA");
    CHECK(s.flatten_pending[0].qty == 276.0);
    CHECK(std::string(s.flatten_pending[0].why) == "KILL SWITCH");
    CHECK(s.flatten_pending[0].requested_ns > 0);
    // And no phantom sell row was invented for a close that did not happen.
    for (const OrderRecord& o : s.orders)
        CHECK(o.side != static_cast<uint8_t>(Side::Sell));
}

TEST_CASE("flatten: a PARTIAL close keeps the expectation open") {
    // A flatten can fill in pieces. The expectation clears on the POSITION
    // reaching flat, never on "a fill arrived" — 100 of 276 closed still leaves
    // 176 shares that nobody has closed, which is the state the whole
    // outstanding-flatten row exists to make visible.
    Engine eng;
    FlatteningBroker broker;
    broker.answer_flatten = false;   // drive the fills by hand instead
    BuyOnceStrat strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.broker = &broker;
    eng.start_live(cfg, {&strat});
    pump_until(eng, [&] { return eng.live_snapshot().orders.size() >= 1; });
    broker.emit(ev_fill(1, 1, Side::Buy, 276.0, 18.07));
    pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty == 276.0; });

    eng.kill_switch();
    pump_until(eng, [&] { return !eng.live_snapshot().flatten_pending.empty(); });
    CHECK(eng.live_snapshot().flatten_pending.size() == 1);

    // 100 of 276 comes back. Still 176 open, so the flatten is NOT done.
    broker.emit(ev_fill(1, 9000, Side::Sell, 100.0, 17.39));
    pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty == 176.0; });
    pump_until(eng, [] { return false; }, 80);
    {
        const LiveSnapshot s = eng.live_snapshot();
        CHECK(s.symbols[0].position.qty == 176.0);
        CHECK(s.flatten_pending.size() == 1);        // still outstanding
        CHECK(s.flatten_pending[0].qty == 276.0);    // what was ASKED for
    }

    // The remainder closes it out; only now does the expectation clear.
    broker.emit(ev_fill(1, 9001, Side::Sell, 176.0, 17.35));
    pump_until(eng, [&] { return eng.live_snapshot().symbols[0].position.qty == 0.0; });
    pump_until(eng, [] { return false; }, 80);
    {
        const LiveSnapshot s = eng.live_snapshot();
        CHECK(s.flatten_pending.empty());
        // Both partials are reconstructed, each under the broker's own id.
        int broker_sells = 0;
        for (const OrderRecord& o : s.orders)
            if (o.side == static_cast<uint8_t>(Side::Sell) && o.broker_originated)
                ++broker_sells;
        CHECK(broker_sells == 2);
    }
    eng.stop_live();
}

// ---------------------------------------------------------------------------
// EngineCtx::bars_to_eod (0.42.0) — the session horizon a stopless strategy
// plans against. Read from the engine, never from the strategy, because a
// strategy that hardcoded 15.95 would recreate the 0.27.0 defect exactly: an
// entry gate reading the calendar against a backstop comparing to a literal,
// 2h57m apart on a 13:00 early close.
// ---------------------------------------------------------------------------
namespace {
// Asks the question at a FIXED timestamp on its first tick, so the answer does
// not depend on when the suite happens to run.
struct HorizonProbe : IStrategy {
    int64_t ask_ts = 0;
    std::atomic<int> answer{-1};
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        if (answer.load() < 0) answer = ctx.bars_to_eod(sid, ask_ts);
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

int64_t local_ns(int y, int mon, int day, int hour, int min) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm)) * 1'000'000'000LL;
}

// Run a live session whose forced-flatten hour is `flat_h` and ask the horizon
// at `ask` local time, on `bar_sec` bars.
int horizon(double flat_h, int hour, int min, int bar_sec = 300) {
    Engine eng;
    StubBroker broker;
    HorizonProbe probe;
    probe.ask_ts = local_ns(2026, 9, 8, hour, min);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = bar_sec;
    cfg.broker = &broker;
    if (flat_h != 0.0)
        cfg.eod_flatten_h_for_day = [flat_h](const std::tm&) { return flat_h; };
    eng.start_live(cfg, {&probe});
    pump_until(eng, [&] { return probe.answer.load() >= 0; });
    const int a = probe.answer.load();
    eng.stop_live();
    return a;
}
} // namespace

TEST_CASE("bars_to_eod: counts the bars actually left before the flatten") {
    // 15:57 cutoff, asked at 15:20 on 5-minute bars: 37 minutes = 7.4 bars.
    // FLOOR, because a partial bar is not one the time stop can count — and
    // rounding up hands back the single bar that makes an unexitable trade look
    // exitable. This is the SNXX shape: 7 left, time_stop 12.
    CHECK(horizon(15.95, 15, 20) == 7);
    // Mid-morning, plenty of room.
    CHECK(horizon(15.95, 10, 0) == 71);
    // Bar size is respected, not assumed: the same instant on 1-minute bars.
    CHECK(horizon(15.95, 15, 20, 60) == 37);
}

TEST_CASE("bars_to_eod: an EARLY CLOSE shortens the horizon") {
    // The 0.27.0 defect, from the other side. On a 13:00 close the flatten is
    // 12:57, so a 12:30 entry has 5 bars — not the 65 a hardcoded 15:57 would
    // report. Three hours of difference on the one question a stopless strategy
    // uses to decide whether it can close what it opens.
    CHECK(horizon(12.95, 12, 30) == 5);
    CHECK(horizon(15.95, 12, 30) == 41);   // the same instant on a full day
}

TEST_CASE("bars_to_eod: past the cutoff there is no room at all") {
    CHECK(horizon(15.95, 15, 57) == 0);
    CHECK(horizon(15.95, 16, 30) == 0);
}

TEST_CASE("bars_to_eod: a live session ALWAYS has a horizon") {
    // No calendar hook installed still answers 15:57, because the engine's
    // backstop is not optional — it fires on every live session whether or not
    // anyone supplied a calendar. A strategy must never be told it has all the
    // room in the world on a route that will liquidate it at 15:57.
    CHECK(horizon(0.0, 15, 20) == 7);
    // kNoEodLimit is reserved for "there is genuinely no cutoff": a day the
    // calendar says never opens (the hook returns <= 0), and the manual
    // backtest path where BacktestConfig::eod_flatten_h is left at 0 — see the
    // acceptance cases in test_strategies.cpp, where an unreachable time stop
    // trades freely without the boundary and is refused with it.
    CHECK(horizon(-1.0, 10, 0) == IStrategyContext::kNoEodLimit);
}

TEST_CASE("bars_to_eod: each symbol is measured in ITS OWN bar size") {
    // The lineup runs symbols at different intervals, and the horizon is
    // consumed as a BAR count against that symbol's `time_stop`. Measuring a
    // 1-minute symbol in 5-minute bars understates its room fivefold and
    // refuses entries that had plenty; the reverse lets through the trades this
    // whole gate exists to stop.
    Engine eng;
    StubBroker broker;
    HorizonProbe slow, fast;
    slow.ask_ts = fast.ask_ts = local_ns(2026, 9, 8, 15, 20);   // 37 min left
    LiveConfig cfg;
    cfg.symbols = {"SLOW", "FAST"};
    cfg.bar_seconds = 300;
    cfg.symbol_bar_seconds = {300, 60};
    cfg.broker = &broker;
    cfg.eod_flatten_h_for_day = [](const std::tm&) { return 15.95; };
    eng.start_live(cfg, {&slow, &fast});
    const bool got = pump_until(eng, [&] {
        eng.push_live_tick("SLOW", 1, 10.0, 0.0);
        eng.push_live_tick("FAST", 2, 10.0, 0.0);
        return slow.answer.load() >= 0 && fast.answer.load() >= 0;
    });
    CHECK(got);
    CHECK(slow.answer.load() == 7);     // 37 min / 5 min
    CHECK(fast.answer.load() == 37);    // 37 min / 1 min
    eng.stop_live();
}
