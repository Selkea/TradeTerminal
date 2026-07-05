// trade_updates JSON -> AlpacaTradeUpdate translation, no network required.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/alpaca_broker.h"

#include <string>
#include <vector>

using tt::AlpacaTradeUpdate;
using tt::alpaca_parse_trade_update;

namespace {
const std::vector<std::string> kSymbols = {"AAPL", "MSFT"};

std::string update(const std::string& event, const std::string& symbol,
                   const std::string& client_id, const std::string& extra_data = "") {
    return R"({"stream":"trade_updates","data":{"event":")" + event + R"(")" +
           extra_data +
           R"(,"order":{"id":"904837e3-3b76-47ec-b432-046db621571b","client_order_id":")" +
           client_id + R"(","symbol":")" + symbol + R"(","side":"buy"}}})";
}
} // namespace

TEST_CASE("fill event carries price, qty, timestamp, and our order id") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(
        update("fill", "AAPL", "tt-1751560000000-7",
               R"(,"price":"213.45","qty":"10","timestamp":"2026-07-02T14:30:01.5Z")"),
        kSymbols, tu));
    CHECK(tu.kind == AlpacaTradeUpdate::Fill);
    CHECK(tu.local_id == 7);
    CHECK(tu.symbol_id == 1);
    CHECK(tu.side == tt::Side::Buy);
    CHECK(tu.price == doctest::Approx(213.45));
    CHECK(tu.qty == doctest::Approx(10.0));
    // 2026-07-02T14:30:01.5Z = 1782950400 (midnight) + 52201s
    CHECK(tu.ts_ns == 1783002601'500'000'000LL);
    CHECK(tu.broker_id == "904837e3-3b76-47ec-b432-046db621571b");
}

TEST_CASE("partial_fill is a fill; numbers may be JSON numbers not strings") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(
        update("partial_fill", "MSFT", "tt-1751560000000-3",
               R"(,"price":501.25,"qty":4,"timestamp":"2026-07-02T14:30:01-04:00")"),
        kSymbols, tu));
    CHECK(tu.kind == AlpacaTradeUpdate::Fill);
    CHECK(tu.symbol_id == 2);
    CHECK(tu.price == doctest::Approx(501.25));
    CHECK(tu.qty == doctest::Approx(4.0));
    // Offset applies: 14:30:01-04:00 == 18:30:01Z.
    CHECK(tu.ts_ns == 1783017001'000'000'000LL);
}

TEST_CASE("canceled and rejected map to their kinds") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(update("canceled", "AAPL", "tt-1-9"), kSymbols, tu));
    CHECK(tu.kind == AlpacaTradeUpdate::Cancel);
    CHECK(tu.local_id == 9);

    REQUIRE(alpaca_parse_trade_update(update("rejected", "AAPL", "tt-1-9"), kSymbols, tu));
    CHECK(tu.kind == AlpacaTradeUpdate::Reject);
}

TEST_CASE("new ack exposes the broker uuid for the cancel map") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(update("new", "AAPL", "tt-1-2"), kSymbols, tu));
    CHECK(tu.kind == AlpacaTradeUpdate::Ack);
    CHECK(tu.local_id == 2);
    CHECK_FALSE(tu.broker_id.empty());
}

TEST_CASE("orders not placed by this session parse with local_id 0") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(update("fill", "AAPL", "someone-elses-id"),
                                      kSymbols, tu));
    CHECK(tu.local_id == 0);
}

TEST_CASE("symbols outside the session table get symbol_id 0") {
    AlpacaTradeUpdate tu;
    REQUIRE(alpaca_parse_trade_update(update("fill", "TSLA", "tt-1-4"), kSymbols, tu));
    CHECK(tu.symbol_id == 0);
}

TEST_CASE("positions parse with signed qty and session mapping") {
    std::vector<tt::AlpacaPosition> rows;
    REQUIRE(tt::alpaca_parse_positions(
        R"([{"symbol":"AAPL","qty":"10","side":"long","avg_entry_price":"210.5"},)"
        R"({"symbol":"TSLA","qty":"5","side":"short","avg_entry_price":"300"},)"
        R"({"symbol":"MSFT","qty":"0","side":"long","avg_entry_price":"0"}])",
        kSymbols, rows));
    REQUIRE(rows.size() == 2);   // zero-qty row dropped
    CHECK(rows[0].symbol_id == 1);
    CHECK(rows[0].qty == doctest::Approx(10));
    CHECK(rows[0].avg_price == doctest::Approx(210.5));
    CHECK(rows[1].symbol_id == 0);   // TSLA not in session
    CHECK(rows[1].qty == doctest::Approx(-5));   // short -> negative

    rows.clear();
    CHECK_FALSE(tt::alpaca_parse_positions("junk", kSymbols, rows));
    CHECK(tt::alpaca_parse_positions("[]", kSymbols, rows));
    CHECK(rows.empty());
}

TEST_CASE("non-trade_updates streams and junk are ignored") {
    AlpacaTradeUpdate tu;
    CHECK_FALSE(alpaca_parse_trade_update(
        R"({"stream":"authorization","data":{"status":"authorized"}})", kSymbols, tu));
    CHECK_FALSE(alpaca_parse_trade_update(
        R"({"stream":"listening","data":{"streams":["trade_updates"]}})", kSymbols, tu));
    CHECK_FALSE(alpaca_parse_trade_update("not json at all", kSymbols, tu));
    CHECK_FALSE(alpaca_parse_trade_update(update("done_for_day", "AAPL", "tt-1-5"),
                                          kSymbols, tu));
}
