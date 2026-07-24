#pragma once
// Pure symbol-ranking for the daily auto-lineup. Given recent bars for each
// candidate returned by a market scan, rank them by realized volatility
// (ATR% of price) behind price and liquidity gates, and keep the top N. No
// I/O and no engine state, so it is unit-tested in isolation
// (see tests/test_symbol_rank.cpp).

#include <cstddef>
#include <string>
#include <vector>

namespace tt {

// One OHLCV bar of a candidate's recent history (daily bars in practice).
struct RankBar {
    double high = 0, low = 0, close = 0, volume = 0;
};

// A scan candidate and its recent bars (chronological; most recent last).
struct RankCandidate {
    std::string symbol;
    std::vector<RankBar> bars;
};

struct RankParams {
    int atr_len = 14;               // bars in the ATR / dollar-volume windows
    double min_price = 5.0;         // drop names whose last close is below this
    double min_dollar_vol = 20e6;   // liquidity floor: avg close*volume ($/day)
    std::size_t top_n = 6;          // keep at most this many
};

struct RankedSymbol {
    std::string symbol;
    double atr_pct = 0;      // avg true range / last close -- the ranking key
    double dollar_vol = 0;   // avg close*volume over the window
};

// Rank candidates by ATR% (descending) after applying the price + liquidity
// gates. Candidates with too few bars (< atr_len+1, since true range needs a
// prior close) or that fail a gate are dropped. Ties break on symbol so the
// result is deterministic. Returns at most params.top_n entries, most volatile
// first.
std::vector<RankedSymbol> rank_by_volatility(const std::vector<RankCandidate>& cands,
                                             const RankParams& params);

} // namespace tt
