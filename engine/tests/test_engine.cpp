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
