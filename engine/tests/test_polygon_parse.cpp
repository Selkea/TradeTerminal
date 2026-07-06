// Polygon websocket/REST JSON -> normalized feed messages, no network.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/polygon_feed.h"

#include <string>
#include <vector>

using tt::FeedMsg;

namespace {
const std::vector<std::string> kSymbols = {"AAPL", "MSFT"};
}

TEST_CASE("trade and quote events normalize with ms->ns timestamps") {
    std::vector<FeedMsg> out;
    const size_t n = tt::polygon_parse_feed_msgs(
        R"([{"ev":"T","sym":"AAPL","i":"52983525029461","x":4,"p":213.45,"s":100,"t":1783002601500,"z":3},)"
        R"({"ev":"Q","sym":"MSFT","bx":11,"bp":501.2,"bs":2,"ax":12,"ap":501.3,"as":3,"t":1783002601501}])",
        kSymbols, out);
    REQUIRE(n == 2);
    CHECK(out[0].kind == FeedMsg::Trade);
    CHECK(out[0].symbol_id == 1);
    CHECK(out[0].price == doctest::Approx(213.45));
    CHECK(out[0].size == doctest::Approx(100));
    CHECK(out[0].ts_ns == 1783002601500'000'000LL);
    CHECK(out[1].kind == FeedMsg::Quote);
    CHECK(out[1].bid == doctest::Approx(501.2));
    CHECK(out[1].ask == doctest::Approx(501.3));
}

TEST_CASE("status handshake events map to control kinds") {
    std::vector<FeedMsg> out;
    CHECK(tt::polygon_parse_feed_msgs(
              R"([{"ev":"status","status":"connected","message":"Connected Successfully"}])",
              kSymbols, out) == 1);
    CHECK(out.back().kind == FeedMsg::Connected);
    CHECK(tt::polygon_parse_feed_msgs(
              R"([{"ev":"status","status":"auth_success","message":"authenticated"}])",
              kSymbols, out) == 1);
    CHECK(out.back().kind == FeedMsg::Authenticated);
    CHECK(tt::polygon_parse_feed_msgs(
              R"([{"ev":"status","status":"success","message":"subscribed to: T.AAPL"}])",
              kSymbols, out) == 1);
    CHECK(out.back().kind == FeedMsg::Subscription);
    CHECK(tt::polygon_parse_feed_msgs(
              R"([{"ev":"status","status":"auth_failed","message":"authentication failed"}])",
              kSymbols, out) == 1);
    CHECK(out.back().kind == FeedMsg::Error);
    CHECK(out.back().error == "authentication failed");
}

TEST_CASE("unknown symbols get id 0; aggregates and junk are skipped") {
    std::vector<FeedMsg> out;
    REQUIRE(tt::polygon_parse_feed_msgs(
                R"([{"ev":"T","sym":"TSLA","p":300.0,"s":1,"t":1783002601500}])",
                kSymbols, out) == 1);
    CHECK(out[0].symbol_id == 0);
    CHECK(tt::polygon_parse_feed_msgs(
              R"([{"ev":"AM","sym":"AAPL","o":1,"h":2,"l":0.5,"c":1.5,"v":100}])",
              kSymbols, out) == 0);
    CHECK(tt::polygon_parse_feed_msgs("junk", kSymbols, out) == 0);
}

TEST_CASE("REST aggs parse for gap backfill") {
    std::vector<tt::RestBar> bars;
    REQUIRE(tt::polygon_parse_rest_bars(
        R"({"ticker":"AAPL","resultsCount":2,"results":[)"
        R"({"t":1783002600000,"o":100.1,"h":101,"l":99.9,"c":100.5,"v":1200},)"
        R"({"t":1783002660000,"o":100.5,"h":100.8,"l":100.2,"c":100.7,"v":800}]})",
        bars));
    REQUIRE(bars.size() == 2);
    CHECK(bars[0].ts_ns == 1783002600000'000'000LL);
    CHECK(bars[1].close == doctest::Approx(100.7));

    bars.clear();
    CHECK_FALSE(tt::polygon_parse_rest_bars("junk", bars));
    CHECK(tt::polygon_parse_rest_bars(R"({"ticker":"AAPL","resultsCount":0})", bars));
    CHECK(bars.empty());
}
