// Client Portal API JSON -> IBKR adapter structs, no gateway required.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/ibkr_broker.h"
#include "engine/ibkr_feed.h"

#include <string>
#include <vector>

using namespace tt;

TEST_CASE("order response: acceptance carries order_id") {
    IbkrOrderResp r;
    REQUIRE(ibkr_parse_order_response(
        R"([{"order_id":"1799796559","order_status":"Submitted","local_order_id":"tt-1-3"}])",
        r));
    CHECK(r.order_id == "1799796559");
    CHECK(r.reply_id.empty());
}

TEST_CASE("order response: confirmation question carries reply id + text") {
    IbkrOrderResp r;
    REQUIRE(ibkr_parse_order_response(
        R"([{"id":"07a13a5a-4a48","message":["The price exceeds the Percentage constraint of 3%."],"isSuppressed":false}])",
        r));
    CHECK(r.order_id.empty());
    CHECK(r.reply_id == "07a13a5a-4a48");
    CHECK(r.message.find("Percentage constraint") != std::string::npos);
}

TEST_CASE("order response: junk rejected") {
    IbkrOrderResp r;
    CHECK_FALSE(ibkr_parse_order_response("[]", r));
    CHECK_FALSE(ibkr_parse_order_response("not json", r));
}

TEST_CASE("accounts: selectedAccount preferred, array fallback") {
    CHECK(ibkr_parse_first_account(
              R"({"accounts":["DU111111","DU222222"],"selectedAccount":"DU222222"})") ==
          "DU222222");
    CHECK(ibkr_parse_first_account(R"({"accounts":["DU111111"]})") == "DU111111");
    CHECK(ibkr_parse_first_account(R"({})").empty());
}

TEST_CASE("conid: prefers the STK row matching the symbol") {
    const char* body = R"([
      {"conid":"11111","symbol":"AAPL","secType":"OPT"},
      {"conid":"265598","symbol":"AAPL","secType":"STK"},
      {"conid":"99999","symbol":"AAPLX","secType":"STK"}])";
    CHECK(ibkr_parse_conid(body, "AAPL") == 265598);
    CHECK(ibkr_parse_conid(body, "MSFT") == 0);
    CHECK(ibkr_parse_conid("junk", "AAPL") == 0);
}

TEST_CASE("orders list parses id + status") {
    std::vector<IbkrOrderStatus> rows;
    REQUIRE(ibkr_parse_orders(
        R"({"orders":[{"orderId":123,"status":"Submitted"},{"orderId":"456","status":"Cancelled"}],"snapshot":true})",
        rows));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].order_id == "123");
    CHECK(rows[1].status == "Cancelled");
}

TEST_CASE("trades parse with dedupe key, ref, and side") {
    std::vector<IbkrTrade> out;
    REQUIRE(ibkr_parse_trades(
        R"([{"execution_id":"0000e0d5.111","symbol":"AAPL","side":"B","price":"213.45",)"
        R"("size":10,"commission":"1.0","order_ref":"tt-1751-7","trade_time_r":1783002601500}])",
        out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].execution_id == "0000e0d5.111");
    CHECK(out[0].order_ref == "tt-1751-7");
    CHECK(out[0].buy);
    CHECK(out[0].qty == doctest::Approx(10));
    CHECK(out[0].price == doctest::Approx(213.45));
    CHECK(out[0].ts_ms == 1783002601500LL);
}

TEST_CASE("gateway session token parses from /tickle") {
    CHECK(tt::ibkr_parse_session_token(
              R"({"session":"abc123def","ssoExpires":420000,"iserver":{}})") ==
          "abc123def");
    CHECK(tt::ibkr_parse_session_token(R"({})").empty());
    CHECK(tt::ibkr_parse_session_token("junk").empty());
}

TEST_CASE("smd market updates: partial fields, string values, conid from topic") {
    tt::IbkrMdUpdate u;
    REQUIRE(tt::ibkr_parse_md_msg(
        R"({"topic":"smd+265598","conid":265598,"_updated":1783002601500,)"
        R"("31":"213.45","84":"213.44","86":"213.46","7059":"100"})",
        u));
    CHECK(u.kind == tt::IbkrMdUpdate::Market);
    CHECK(u.conid == 265598);
    CHECK(u.has_last);
    CHECK(u.last == doctest::Approx(213.45));
    CHECK(u.bid == doctest::Approx(213.44));
    CHECK(u.ask == doctest::Approx(213.46));
    CHECK(u.size == doctest::Approx(100));
    CHECK(u.ts_ms == 1783002601500LL);

    // Quote-only delta: no last price present.
    u = {};
    REQUIRE(tt::ibkr_parse_md_msg(R"({"topic":"smd+8314","84":"501.20"})", u));
    CHECK(u.conid == 8314);   // recovered from the topic
    CHECK_FALSE(u.has_last);
    CHECK(u.has_bid);

    // System noise is ignored; auth loss is surfaced.
    u = {};
    CHECK_FALSE(tt::ibkr_parse_md_msg(R"({"topic":"system","hb":1783002601500})", u));
    REQUIRE(tt::ibkr_parse_md_msg(R"({"topic":"sts","args":{"authenticated":false}})", u));
    CHECK(u.kind == tt::IbkrMdUpdate::AuthLost);
}

TEST_CASE("history bars parse for gap backfill") {
    std::vector<tt::AlpacaRestBar> bars;
    REQUIRE(tt::ibkr_parse_history_bars(
        R"({"symbol":"AAPL","data":[{"t":1783002600000,"o":100.1,"h":101,"l":99.9,"c":100.5,"v":120}],"points":1})",
        bars));
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].ts_ns == 1783002600000'000'000LL);
    CHECK(bars[0].close == doctest::Approx(100.5));
    CHECK_FALSE(tt::ibkr_parse_history_bars("junk", bars));
}

TEST_CASE("positions parse conid + signed qty") {
    std::vector<IbkrPosition> out;
    REQUIRE(ibkr_parse_positions(
        R"([{"conid":265598,"position":100.0},{"conid":8314,"position":-50.0},)"
        R"({"conid":123,"position":0.0}])",
        out));
    REQUIRE(out.size() == 2);   // flat row dropped
    CHECK(out[0].conid == 265598);
    CHECK(out[1].qty == doctest::Approx(-50));
}
