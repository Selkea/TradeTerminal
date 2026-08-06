#pragma once
// Which symbol loses tick-by-tick when IBKR runs out of subscriptions.
//
// reqTickByTickData is capped per account. Past the cap the feed falls back to
// throttled streaming quotes (see tws_feed.cpp's 10190 handler) — real data,
// but sampled rather than every print. Nothing is wrong with that; what IS
// wrong is that the symbol which gets it is whichever happens to be last in
// the session list, i.e. arbitrary.
//
// It matters because strategies differ in what they read off the tape:
//   - a tick-driven strategy (burst scalping) needs every print
//   - an opening-range or channel strategy reads bar HIGH/LOW, which sampling
//     understates — a narrower range means tighter stops and earlier breakouts
//   - a strategy that only reads bar CLOSE is unaffected: the close is just the
//     last price at the boundary, which sampled quotes still deliver
//
// So order the session list by what each symbol's strategy needs, and let the
// close-only ones take the degraded slots. Observed 2026-08-05: slot 6 landed
// on Bollinger, which reads closes only — harmless, but by luck, and the next
// lineup could just as easily have put an opening-range strategy there.

#include <algorithm>
#include <string>
#include <vector>

namespace tt::ui {

// Lower rank = needs the tape more = keep it in a guaranteed slot.
// Unknown keys rank as tick-sensitive on purpose: a strategy nobody has
// classified must never be silently demoted into the sampled slot.
inline int feed_fidelity_rank(const std::string& strat_key) {
    if (strat_key == "scalper_burst.cpp") return 0;   // every print
    // Close-only: the sampled tape carries the close faithfully.
    if (strat_key == "bollinger_reversion.cpp" || strat_key == "rsi2_pullback.cpp" ||
        strat_key == "sma_crossover.cpp" || strat_key.empty())
        return 2;
    return 1;   // reads bar high/low (orb, donchian), and anything unrecognised
}

// Stable so symbols within a tier keep the caller's order — the live session's
// symbol ids come from this list, and an unstable sort would reshuffle them
// between runs for no reason.
template <class T, class KeyFn>
void order_by_feed_fidelity(std::vector<T>& syms, KeyFn key_of) {
    std::stable_sort(syms.begin(), syms.end(), [&](const T& a, const T& b) {
        return feed_fidelity_rank(key_of(a)) < feed_fidelity_rank(key_of(b));
    });
}

} // namespace tt::ui
