#include "engine/symbol_rank.h"

#include <algorithm>
#include <cmath>

namespace tt {

std::vector<RankedSymbol> rank_by_volatility(const std::vector<RankCandidate>& cands,
                                             const RankParams& params) {
    const int n = params.atr_len > 0 ? params.atr_len : 14;
    std::vector<RankedSymbol> out;
    out.reserve(cands.size());
    for (const auto& c : cands) {
        const auto& b = c.bars;
        // True range needs each bar's prior close, so we need n+1 bars.
        if (static_cast<int>(b.size()) < n + 1) continue;
        double tr_sum = 0, dv_sum = 0;
        const std::size_t start = b.size() - static_cast<std::size_t>(n);
        for (std::size_t i = start; i < b.size(); ++i) {
            const double prev_close = b[i - 1].close;
            const double hl = b[i].high - b[i].low;
            const double hc = std::fabs(b[i].high - prev_close);
            const double lc = std::fabs(b[i].low - prev_close);
            tr_sum += std::max({hl, hc, lc});
            dv_sum += b[i].close * b[i].volume;
        }
        const double last_close = b.back().close;
        const double dollar_vol = dv_sum / n;
        if (last_close < params.min_price) continue;
        if (last_close <= 0) continue;
        if (dollar_vol < params.min_dollar_vol) continue;
        RankedSymbol r;
        r.symbol = c.symbol;
        r.atr_pct = (tr_sum / n) / last_close;
        r.dollar_vol = dollar_vol;
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const RankedSymbol& a, const RankedSymbol& b) {
        if (a.atr_pct != b.atr_pct) return a.atr_pct > b.atr_pct;
        return a.symbol < b.symbol;   // deterministic tie-break
    });
    if (out.size() > params.top_n) out.resize(params.top_n);
    return out;
}

} // namespace tt
