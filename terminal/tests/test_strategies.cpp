// Strategy logic, driven directly through the SDK interface.
//
// The strategies hold the actual trading decisions but had no test coverage —
// every defect in them so far (warmup gates that could never be satisfied,
// sizing knobs the engine discarded, order ids that wedged a symbol) was found
// in production or by reading. These tests drive a promoted strategy against a
// stand-in context so a behaviour can be pinned without a broker or an engine.

#include "doctest.h"

#include "tt/strategy_api.h"
#include "tt/strategy_registry.h"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace tt;

namespace {

// Minimal engine stand-in: records what the strategy submits, and answers
// param/position/budget queries from values the test sets.
struct FakeCtx final : IStrategyContext {
    std::map<std::string, double> params;
    double cash_ = 100'000.0;
    double budget_ = 5'000.0;
    Position pos{};
    int64_t now = 0;
    std::vector<OrderRequest> sent;
    std::vector<uint64_t> cancelled;
    uint64_t next_id = 1;

    uint64_t submit_order(const OrderRequest& r) noexcept override {
        sent.push_back(r);
        return next_id++;
    }
    bool cancel_order(uint64_t id) noexcept override {
        cancelled.push_back(id);
        return true;
    }
    Position position(uint32_t) const noexcept override { return pos; }
    double cash() const noexcept override { return cash_; }
    double budget(uint32_t) const noexcept override { return budget_; }
    int64_t now_ns() const noexcept override { return now; }
    uint32_t symbol_id(const char*) noexcept override { return 1; }
    double param(const char* n, double fallback) const noexcept override {
        const auto it = params.find(n);
        return it != params.end() ? it->second : fallback;
    }
    void log(int, const char*) noexcept override {}
};

// Epoch ns for a LOCAL wall-clock time. Strategies gate on
// hour_of_day_local(), which reads the machine timezone, so a test that hard-
// coded UTC would pass or fail depending on where it ran. mktime interprets
// the same local zone, so this stays correct anywhere.
int64_t local_ts(int y, int mon, int day, int hour, int min) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_isdst = -1;   // let the C library decide DST for this date
    return static_cast<int64_t>(std::mktime(&tm)) * 1'000'000'000LL;
}

IStrategy* make(const char* key) {
    const StaticStrategyEntry* e = find_static_strategy(key);
    REQUIRE(e != nullptr);
    return e->create();
}

Bar mk_bar(int64_t ts, double close) {
    return Bar{ts, close, close, close, close, 1000.0};
}

// Did the strategy send a market sell for the whole 100-share position?
bool saw_flatten(const FakeCtx& ctx) {
    for (const OrderRequest& r : ctx.sent)
        if (r.side == Side::Sell && r.type == OrdType::Market && r.qty == 100.0)
            return true;
    return false;
}

// A rise that ends in a two-bar dip: close stays above the trend SMA (regime
// filter passes) while RSI(2) collapses (entry threshold passes).
std::vector<double> pullback_series() {
    std::vector<double> px;
    for (int i = 0; i < 28; ++i) px.push_back(100.0 + i);   // 100 -> 127
    px.push_back(125.0);                                    // two down bars:
    px.push_back(123.0);                                    // RSI(2) -> ~0
    return px;
}

// Feed rsi2_pullback a pullback series whose first bar lands at `start_hour`
// local, one bar every `bar_sec`. Returns how many orders it submitted.
size_t rsi2_orders_from(int start_hour, int start_min, int bar_sec) {
    IStrategy* s = make("rsi2_pullback.cpp");
    FakeCtx ctx;
    ctx.params["rsi_len"] = 2;
    ctx.params["buy_below"] = 40;   // generous: the test is about the gate
    ctx.params["exit_ma"] = 2;
    ctx.params["trend_ma"] = 20;
    s->on_init(ctx);

    const std::vector<double> px = pullback_series();
    int64_t ts = local_ts(2026, 8, 5, start_hour, start_min);
    for (double p : px) {
        s->on_bar(ctx, 1, mk_bar(ts, p));
        ts += static_cast<int64_t>(bar_sec) * 1'000'000'000LL;
    }
    const size_t n = ctx.sent.size();
    s->destroy();
    return n;
}

} // namespace

// ---- rsi2_pullback: the entry window -------------------------------------
// On 2026-08-04 this strategy signalled at 04:37 on a 5m bar built from thin
// pre-market prints. IBKR held the order PreSubmitted for five hours and filled
// it at the open, at a price the strategy never saw. It was the only shipped
// strategy with no time-of-day gate.

TEST_CASE("rsi2: takes the entry inside regular hours") {
    // Positive control — without this the gate tests below prove nothing.
    CHECK(rsi2_orders_from(9, 35, 300) == 1);
}

TEST_CASE("rsi2: refuses the same entry pre-market") {
    // Identical price series, only the clock differs.
    CHECK(rsi2_orders_from(3, 0, 300) == 0);
}

TEST_CASE("rsi2: refuses the same entry after the close") {
    CHECK(rsi2_orders_from(17, 0, 300) == 0);
}

TEST_CASE("rsi2: the window is ignored on daily bars") {
    // A daily bar is stamped at the session date, so applying a time-of-day
    // window would silently turn every daily backtest into a no-trade run.
    CHECK(rsi2_orders_from(0, 0, 86'400) == 1);
}

// ---- orb_breakout: the EOD flatten backstop -------------------------------
// The bar-count EOD test is only as good as session_min. Swept above the real
// session length it never trips, and an intraday strategy holds through the
// close.

TEST_CASE("orb: flattens on the clock even when session_min outlives the day") {
    IStrategy* s = make("orb_breakout.cpp");
    FakeCtx ctx;
    // session_min far past a real day: minutes >= 1440-5 is unreachable in a
    // 390-minute session, so ONLY the clock backstop can flatten this.
    ctx.params["session_min"] = 1440;
    ctx.params["eod_min"] = 5;
    ctx.params["range_min"] = 15;
    s->on_init(ctx);

    // First bar flat, so the new-session rollover doesn't flatten anything.
    int64_t ts = local_ts(2026, 8, 5, 9, 35);
    s->on_bar(ctx, 1, mk_bar(ts, 100.0));
    ctx.sent.clear();
    ctx.cancelled.clear();

    // Now hold a long through the day and run to the close.
    ctx.pos = Position{1, 100.0, 100.0, 0.0, 0.0};
    for (int i = 1; i <= 78; ++i) {   // 09:40 -> 16:05, 5-minute bars
        ts += 300LL * 1'000'000'000LL;
        s->on_bar(ctx, 1, mk_bar(ts, 100.0));
    }

    CHECK(saw_flatten(ctx));
    s->destroy();
}

// ---- bollinger_reversion: "reverted" must mean the price came back ---------
// z is a distance from a ROLLING mean, so it recovers either because the price
// rose or because the mean fell to meet the price. Only the first is a
// reversion. SPCH, 2026-08-05: bought 770 @ 6.49, price fell 6.3%, and the
// strategy exited "reverted to band" at 6.08 for -$315.70 — reporting a failed
// trade as a success.
namespace {
// Drives bollinger through a dip entry and then `after` more closes, returning
// the orders it sent. length is deliberately short (5, as SPCH ran) so the mean
// catches up to the price within a few bars.
std::vector<OrderRequest> boll_run(const std::vector<double>& after,
                                   bool fill_entry_at) {
    IStrategy* s = make("bollinger_reversion.cpp");
    FakeCtx ctx;
    ctx.params["length"] = 5;
    ctx.params["entry_z"] = 1.5;
    ctx.params["exit_z"] = 0.58;
    ctx.params["time_stop"] = 500;   // far away: isolate the reversion test
    ctx.params["trend_len"] = 0;     // trend filter off
    s->on_init(ctx);

    int64_t ts = local_ts(2026, 8, 5, 10, 0);
    auto bar = [&](double px) {
        s->on_bar(ctx, 1, mk_bar(ts, px));
        ts += 300LL * 1'000'000'000LL;
    };
    // Flat, steady, then a sharp drop -> z far below the mean -> entry.
    for (int i = 0; i < 12; ++i) bar(100.0);
    bar(96.0);
    REQUIRE(ctx.sent.size() == 1);            // the entry went out
    const double entry_px = 96.0;
    ctx.sent.clear();
    if (fill_entry_at) {
        Fill f{1, 1, Side::Buy, {}, ts, entry_px, 10.0, 0.0};
        ctx.pos = Position{1, 10.0, entry_px, 0.0, 0.0};
        s->on_fill(ctx, f);
    } else {
        ctx.pos = Position{1, 10.0, entry_px, 0.0, 0.0};   // adopted: no fill seen
    }
    for (double px : after) bar(px);
    const std::vector<OrderRequest> out = ctx.sent;
    s->destroy();
    return out;
}
} // namespace

TEST_CASE("bollinger: does not call it a reversion when the mean fell to the price") {
    // The SPCH shape: price drops, then STABILISES below the entry. The 5-bar
    // mean walks down to meet it, z climbs back over -exit_z, and the pre-fix
    // code sold here — underwater — calling it a reversion. (A price that keeps
    // falling keeps z negative; it is the flattening that fakes the recovery.)
    const auto orders = boll_run({95.5, 95.2, 95.0, 95.3, 95.1, 95.2, 95.0}, true);
    CHECK(orders.empty());
}

TEST_CASE("bollinger: exits when the price genuinely recovers") {
    // Positive control: same mechanism, but the price comes back above entry.
    const auto orders = boll_run({97.0, 98.5, 99.5, 100.0, 100.5}, true);
    REQUIRE(orders.size() >= 1);
    CHECK(orders[0].side == Side::Sell);
}

TEST_CASE("bollinger: an adopted position still exits on z alone") {
    // No fill of ours behind the position (hot restart adopted it), so there is
    // no entry price to judge against — fall back to the old behaviour rather
    // than hold something we cannot reason about.
    const auto orders = boll_run({95.5, 95.2, 95.0, 95.3, 95.1, 95.2, 95.0}, false);
    CHECK_FALSE(orders.empty());
}

TEST_CASE("bollinger: a stored time_stop of 0 is repaired, not obeyed") {
    // The optimizer swept time_stop to 0 on SPCH. With no price stop by design,
    // 0 leaves a losing position with nothing to close it at all.
    IStrategy* s = make("bollinger_reversion.cpp");
    FakeCtx ctx;
    ctx.params["length"] = 5;
    ctx.params["entry_z"] = 1.5;
    ctx.params["exit_z"] = 0.58;
    ctx.params["time_stop"] = 0;     // the value that shipped live
    ctx.params["trend_len"] = 0;
    s->on_init(ctx);

    int64_t ts = local_ts(2026, 8, 5, 10, 0);
    auto bar = [&](double px) {
        s->on_bar(ctx, 1, mk_bar(ts, px));
        ts += 300LL * 1'000'000'000LL;
    };
    for (int i = 0; i < 12; ++i) bar(100.0);
    bar(96.0);
    REQUIRE(ctx.sent.size() == 1);
    Fill f{1, 1, Side::Buy, {}, ts, 96.0, 10.0, 0.0};
    ctx.pos = Position{1, 10.0, 96.0, 0.0, 0.0};
    s->on_fill(ctx, f);
    ctx.sent.clear();

    // Price never recovers, so only a time stop can close this. With the stored
    // 0 obeyed it would ride forever; repaired to 12 bars it must exit.
    for (int i = 0; i < 20; ++i) bar(90.0);
    REQUIRE_FALSE(ctx.sent.empty());
    CHECK(ctx.sent[0].side == Side::Sell);
    s->destroy();
}

TEST_CASE("orb: the backstop does not fire before the close") {
    IStrategy* s = make("orb_breakout.cpp");
    FakeCtx ctx;
    ctx.params["session_min"] = 1440;
    ctx.params["eod_min"] = 5;
    s->on_init(ctx);

    int64_t ts = local_ts(2026, 8, 5, 9, 35);
    s->on_bar(ctx, 1, mk_bar(ts, 100.0));
    ctx.pos = Position{1, 100.0, 100.0, 0.0, 0.0};
    ctx.sent.clear();

    for (int i = 1; i <= 24; ++i) {   // only to ~11:35
        ts += 300LL * 1'000'000'000LL;
        s->on_bar(ctx, 1, mk_bar(ts, 100.0));
    }
    CHECK_FALSE(saw_flatten(ctx));
    s->destroy();
}
