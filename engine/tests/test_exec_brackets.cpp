// Stop orders and bracket (OCO) exits in the fill simulator.
#include "doctest.h"

#include "engine/exec_sim.h"

#include <vector>

using namespace tt;

namespace {
ExecParams instant() {
    ExecParams p;
    p.latency_ns = 0;
    p.latency_jitter_ns = 0;
    p.slippage_bps = 0;
    p.fee_per_share = 0;
    p.min_fee = 0;
    return p;
}
} // namespace

TEST_CASE("stop order triggers only through the stop price") {
    ExecSim ex(instant());
    // Sell stop at 95: protective stop under a long.
    const uint64_t id = ex.submit({1, Side::Sell, OrdType::Stop, {}, 10, 0, 95.0, 0, 0}, 0);
    REQUIRE(id != 0);
    std::vector<Fill> fills;
    ex.on_price(1, 96.0, 1, fills);
    CHECK(fills.empty());          // above the stop: no trigger
    ex.on_price(1, 94.5, 2, fills);
    REQUIRE(fills.size() == 1);    // through the stop: fills
    CHECK(fills[0].order_id == id);
    CHECK(fills[0].price == doctest::Approx(94.5));
}

TEST_CASE("stop order rejects a zero stop price") {
    ExecSim ex(instant());
    CHECK(ex.submit({1, Side::Sell, OrdType::Stop, {}, 10, 0, 0, 0, 0}, 0) == 0);
}

TEST_CASE("bracket spawns OCO exits; take-profit cancels the stop leg") {
    ExecSim ex(instant());
    // Buy 10 @ market with TP 110 / SL 90.
    REQUIRE(ex.submit({1, Side::Buy, OrdType::Market, {}, 10, 0, 0, 110.0, 90.0}, 0) != 0);
    std::vector<Fill> fills;
    ex.on_price(1, 100.0, 1, fills);
    REQUIRE(fills.size() == 1);        // parent filled
    CHECK(ex.open_orders() == 2);      // TP + SL legs live

    fills.clear();
    ex.on_price(1, 111.0, 2, fills);   // through the TP
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].side == Side::Sell);
    CHECK(fills[0].price == doctest::Approx(111.0));   // price improvement
    CHECK(ex.open_orders() == 0);      // OCO: stop leg cancelled
}

TEST_CASE("bracket stop-loss leg fills when price collapses") {
    ExecSim ex(instant());
    REQUIRE(ex.submit({1, Side::Buy, OrdType::Market, {}, 10, 0, 0, 110.0, 90.0}, 0) != 0);
    std::vector<Fill> fills;
    ex.on_price(1, 100.0, 1, fills);
    fills.clear();
    ex.on_price(1, 89.0, 2, fills);    // through the SL
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].side == Side::Sell);
    CHECK(ex.open_orders() == 0);      // TP leg cancelled
}

TEST_CASE("cancelling one bracket leg cancels the group") {
    ExecSim ex(instant());
    REQUIRE(ex.submit({1, Side::Buy, OrdType::Market, {}, 10, 0, 0, 110.0, 90.0}, 0) != 0);
    std::vector<Fill> fills;
    ex.on_price(1, 100.0, 1, fills);
    REQUIRE(ex.open_orders() == 2);
    // Cancel whichever leg got the first id after the parent.
    CHECK(ex.cancel(2));
    CHECK(ex.open_orders() == 0);
}
