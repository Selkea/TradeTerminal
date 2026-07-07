// Finnhub websocket JSON -> normalized feed messages, no network.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/finnhub_feed.h"

#include <string>
#include <vector>

using tt::FeedMsg;

namespace {
const std::vector<std::string> kSymbols = {"AAPL", "MSFT"};
}

TEST_CASE("trade events normalize with ms->ns timestamps") {
    std::vector<FeedMsg> out;
    const size_t n = tt::finnhub_parse_feed_msgs(
        R"({"type":"trade","data":[)"
        R"({"s":"AAPL","p":213.45,"t":1783002601500,"v":100,"c":["1","12"]},)"
        R"({"s":"MSFT","p":501.3,"t":1783002601501,"v":5}]})",
        kSymbols, out);
    REQUIRE(n == 2);
    CHECK(out[0].kind == FeedMsg::Trade);
    CHECK(out[0].symbol_id == 1);
    CHECK(out[0].price == doctest::Approx(213.45));
    CHECK(out[0].size == doctest::Approx(100));
    CHECK(out[0].ts_ns == 1783002601500'000'000LL);
    CHECK(out[1].kind == FeedMsg::Trade);
    CHECK(out[1].symbol_id == 2);
    CHECK(out[1].price == doctest::Approx(501.3));
    CHECK(out[1].size == doctest::Approx(5));
}

TEST_CASE("odd lot may omit size; size defaults to 0") {
    std::vector<FeedMsg> out;
    REQUIRE(tt::finnhub_parse_feed_msgs(
                R"({"type":"trade","data":[{"s":"AAPL","p":213.5,"t":1783002601600}]})",
                kSymbols, out) == 1);
    CHECK(out[0].symbol_id == 1);
    CHECK(out[0].price == doctest::Approx(213.5));
    CHECK(out[0].size == doctest::Approx(0));
}

TEST_CASE("error frames map to Error with the message") {
    std::vector<FeedMsg> out;
    REQUIRE(tt::finnhub_parse_feed_msgs(
                R"({"type":"error","msg":"Invalid API key"})", kSymbols, out) == 1);
    CHECK(out.back().kind == FeedMsg::Error);
    CHECK(out.back().error == "Invalid API key");
}

TEST_CASE("ping, unknown symbols, and junk are skipped") {
    std::vector<FeedMsg> out;
    CHECK(tt::finnhub_parse_feed_msgs(R"({"type":"ping"})", kSymbols, out) == 0);
    CHECK(tt::finnhub_parse_feed_msgs(
              R"({"type":"trade","data":[{"s":"TSLA","p":300.0,"t":1,"v":1}]})",
              kSymbols, out) == 0);   // symbol not in this session's table
    CHECK(tt::finnhub_parse_feed_msgs("junk", kSymbols, out) == 0);
}
