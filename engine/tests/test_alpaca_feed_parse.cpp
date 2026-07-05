// Market-data stream JSON -> AlpacaFeedMsg translation, no network required.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/alpaca_feed.h"

#include <string>
#include <vector>

using tt::AlpacaFeedMsg;
using tt::alpaca_parse_feed_msgs;

namespace {
const std::vector<std::string> kSymbols = {"AAPL", "MSFT"};
}

TEST_CASE("trade and quote in one array parse in order") {
    std::vector<AlpacaFeedMsg> out;
    const size_t n = alpaca_parse_feed_msgs(
        R"([{"T":"t","S":"AAPL","p":213.45,"s":100,"t":"2026-07-02T14:30:01.5Z"},)"
        R"({"T":"q","S":"MSFT","bp":501.2,"ap":501.3,"bs":2,"as":3,"t":"2026-07-02T14:30:01Z"}])",
        kSymbols, out);
    REQUIRE(n == 2);
    CHECK(out[0].kind == AlpacaFeedMsg::Trade);
    CHECK(out[0].symbol_id == 1);
    CHECK(out[0].price == doctest::Approx(213.45));
    CHECK(out[0].size == doctest::Approx(100.0));
    CHECK(out[0].ts_ns == 1783002601'500'000'000LL);
    CHECK(out[1].kind == AlpacaFeedMsg::Quote);
    CHECK(out[1].symbol_id == 2);
    CHECK(out[1].bid == doctest::Approx(501.2));
    CHECK(out[1].ask == doctest::Approx(501.3));
}

TEST_CASE("handshake control messages") {
    std::vector<AlpacaFeedMsg> out;
    CHECK(alpaca_parse_feed_msgs(R"([{"T":"success","msg":"connected"}])", kSymbols, out) == 1);
    CHECK(out.back().kind == AlpacaFeedMsg::Connected);
    CHECK(alpaca_parse_feed_msgs(R"([{"T":"success","msg":"authenticated"}])", kSymbols, out) == 1);
    CHECK(out.back().kind == AlpacaFeedMsg::Authenticated);
    CHECK(alpaca_parse_feed_msgs(
              R"([{"T":"subscription","trades":["AAPL"],"quotes":["AAPL"]}])", kSymbols,
              out) == 1);
    CHECK(out.back().kind == AlpacaFeedMsg::Subscription);
}

TEST_CASE("errors carry the message and code") {
    std::vector<AlpacaFeedMsg> out;
    REQUIRE(alpaca_parse_feed_msgs(
                R"([{"T":"error","code":406,"msg":"connection limit exceeded"}])", kSymbols,
                out) == 1);
    CHECK(out[0].kind == AlpacaFeedMsg::Error);
    CHECK(out[0].error == "connection limit exceeded (code 406)");
}

TEST_CASE("unknown symbols get id 0; unknown types and junk are skipped") {
    std::vector<AlpacaFeedMsg> out;
    REQUIRE(alpaca_parse_feed_msgs(
                R"([{"T":"t","S":"TSLA","p":300.0,"s":1,"t":"2026-07-02T14:30:01Z"}])",
                kSymbols, out) == 1);
    CHECK(out[0].symbol_id == 0);
    CHECK(alpaca_parse_feed_msgs(R"([{"T":"b","S":"AAPL","o":1,"h":2,"l":0.5,"c":1.5}])",
                                 kSymbols, out) == 0);
    CHECK(alpaca_parse_feed_msgs("not json", kSymbols, out) == 0);
    CHECK(alpaca_parse_feed_msgs(R"({"T":"t"})", kSymbols, out) == 0);   // not an array
}
