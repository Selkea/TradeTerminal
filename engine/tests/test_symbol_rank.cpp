#include "doctest.h"

#include "engine/symbol_rank.h"

using namespace tt;

namespace {
// n bars with a constant daily range around price px and constant volume.
// Constant close makes ATR == range, so ATR% == range/px -- easy to reason
// about the expected ordering.
RankCandidate make_cand(const std::string& sym, int n, double px, double range,
                        double vol) {
    RankCandidate c;
    c.symbol = sym;
    for (int i = 0; i < n; ++i)
        c.bars.push_back({px + range / 2, px - range / 2, px, vol});
    return c;
}
} // namespace

TEST_CASE("rank_by_volatility orders by ATR% descending") {
    RankParams p;
    p.atr_len = 14;
    p.min_price = 5;
    p.min_dollar_vol = 1e6;
    p.top_n = 6;
    std::vector<RankCandidate> cands = {
        make_cand("LOW", 20, 100, 1.0, 1'000'000),    // 1% range
        make_cand("HIGH", 20, 100, 5.0, 1'000'000),   // 5% range
        make_cand("MID", 20, 100, 3.0, 1'000'000),    // 3% range
    };
    auto r = rank_by_volatility(cands, p);
    REQUIRE(r.size() == 3);
    CHECK(r[0].symbol == "HIGH");
    CHECK(r[1].symbol == "MID");
    CHECK(r[2].symbol == "LOW");
    CHECK(r[0].atr_pct > r[1].atr_pct);
    CHECK(r[0].atr_pct == doctest::Approx(0.05));
}

TEST_CASE("price gate drops sub-min-price names") {
    RankParams p;
    p.min_price = 5;
    p.min_dollar_vol = 0;
    p.top_n = 10;
    std::vector<RankCandidate> cands = {
        make_cand("PENNY", 20, 2.0, 0.5, 1'000'000),   // $2 < $5 gate
        make_cand("OK", 20, 50, 2.0, 1'000'000),
    };
    auto r = rank_by_volatility(cands, p);
    REQUIRE(r.size() == 1);
    CHECK(r[0].symbol == "OK");
}

TEST_CASE("liquidity gate drops thin names") {
    RankParams p;
    p.min_price = 1;
    p.min_dollar_vol = 20e6;
    p.top_n = 10;
    std::vector<RankCandidate> cands = {
        make_cand("THIN", 20, 50, 2.0, 1'000),           // 50*1k = 50k << 20M
        make_cand("LIQUID", 20, 50, 2.0, 1'000'000),     // 50*1M = 50M
    };
    auto r = rank_by_volatility(cands, p);
    REQUIRE(r.size() == 1);
    CHECK(r[0].symbol == "LIQUID");
}

TEST_CASE("top_n truncates to the most volatile") {
    RankParams p;
    p.min_price = 1;
    p.min_dollar_vol = 0;
    p.top_n = 2;
    std::vector<RankCandidate> cands = {
        make_cand("A", 20, 100, 5.0, 1'000'000),
        make_cand("B", 20, 100, 4.0, 1'000'000),
        make_cand("C", 20, 100, 3.0, 1'000'000),
        make_cand("D", 20, 100, 2.0, 1'000'000),
    };
    auto r = rank_by_volatility(cands, p);
    REQUIRE(r.size() == 2);
    CHECK(r[0].symbol == "A");
    CHECK(r[1].symbol == "B");
}

TEST_CASE("candidates with too few bars are skipped") {
    RankParams p;
    p.atr_len = 14;
    p.min_price = 1;
    p.min_dollar_vol = 0;
    p.top_n = 10;
    std::vector<RankCandidate> cands = {
        make_cand("SHORT", 10, 100, 5.0, 1'000'000),   // 10 bars < 14+1
        make_cand("LONG", 20, 100, 2.0, 1'000'000),
    };
    auto r = rank_by_volatility(cands, p);
    REQUIRE(r.size() == 1);
    CHECK(r[0].symbol == "LONG");
}

TEST_CASE("empty input yields empty output") {
    RankParams p;
    CHECK(rank_by_volatility({}, p).empty());
}
