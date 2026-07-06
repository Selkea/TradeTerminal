// Client Portal API JSON -> IBKR adapter structs, no gateway required.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/ibkr_broker.h"

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
