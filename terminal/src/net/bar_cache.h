#pragma once

// A short-lived cache of delivered historical bar series, so one series is
// fetched from IB ONCE per build instead of once per consumer.
//
// WHY THIS EXISTS. The 2026-08-10 daily lineup build took 1543 s (09:35:00 ->
// 10:00:43) and the accounting closes exactly: 1501 s of it (97.3%) was dead
// IO wait, 26 s was productive IO wait, and 15 s was compute. The engine's
// total backtest CPU across all 582 backtests in that build was 3398 ms
// (mean 5.84 ms) — the build was not slow because of arithmetic, it was slow
// because it asked IB for the same bars over and over and then waited.
//
// A tournament races 5 candidate strategies over ONE symbol's ONE series, and
// pump_tournament's Phase::Launch calls queue_sweep() per candidate, each of
// which issues a fresh reqHistoricalData. SSPC 5m/6mo was fetched FOUR times
// in that build — deliveries at 09:39:21, 09:39:33, 09:40:36 and 09:40:41.
// The first two are 12 SECONDS apart, which breaks IB's documented rule against
// re-issuing an identical historical request inside 15 s. ~60 historical
// requests went out against IB's 60-per-10-minutes budget, the data session
// went silent at 09:40:41, and reqIds 45-64 produced nothing but cancel/retry
// lines for the next twenty minutes.
//
// So the duplicate fetches were not merely wasted time; they were what spent
// the pacing budget that the rest of the build then had none of.
//
// SINGLE-THREADED BY DESIGN. TwsData::Io owns one of these and touches it only
// from the I/O thread (same ownership as Io::hist), so it carries no mutex.
// Anything reading it from the UI thread would need one — don't.

#include "market_data.h"      // tt::Candle
#include "net/hist_pacing.h"  // hist_key: one definition of "the same request"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tt::net {

// How long a delivered series may be replayed instead of re-fetched.
//
// Five minutes, bounded from both sides:
//
//  - It must comfortably outlive ONE symbol's tournament, because that is the
//    duplication this cache exists to remove. The five candidates for a symbol
//    are launched back to back and, once they stop waiting on IB, a whole
//    tournament is seconds of engine time (5 candidates x ~110 backtests x
//    5.84 ms). Five minutes covers even a badly degraded one.
//  - It must NOT outlive the autopilot's re-optimize cadence, which is 30
//    minutes in production. A re-optimization exists to score the strategy on
//    data it has not seen; serving it a cached series would make the cycle a
//    no-op that reports success. Five minutes is well clear of that.
//  - On the production interval (5m bars) it is at most one unfinished bar of
//    staleness, and the optimizer scores months of history.
//
// Entries are ALSO dropped wholesale on a data-session teardown (see
// TwsData::Io::drop_connection): bars belong to the session that delivered
// them, the same rule HistoryFreshness::clear() and GatewayAuth::session_lost()
// already follow. That is what stops a series being reused across sessions —
// the TTL alone would happily do it for five minutes.
inline constexpr int64_t kBarCacheTtlMs = 5 * 60 * 1000;

// Memory bound. A 5m/6mo series is ~9500 bars and sizeof(Candle) is 48, so one
// series is ~456 KB; a lineup build holds ~6 of those plus 30 tiny 1d/1mo
// series. 250k candles is ~12 MB, roughly 26 full-size series — several times
// what a build needs, and a hard ceiling regardless of what asks for what.
inline constexpr size_t kBarCacheMaxCandles = 250'000;
// Second bound, so a flood of TINY series (the 30-symbol 1d/1mo ranking pass is
// 21 bars each) cannot grow the map without limit while staying far under the
// candle ceiling.
inline constexpr size_t kBarCacheMaxEntries = 64;

// Not cached at all: a delivery too thin for anything downstream to use.
//
// App rejects a series under 3 bars in both consumers (start_pending_backtest
// and pump_sweep log "not enough data"), so a 1- or 2-bar delivery is "nothing
// usable" by the app's own definition. Caching it would turn one bad answer —
// a halted or newly-listed pick, or a partial answer during a data-farm hiccup —
// into five minutes of confidently-served nothing, and it would defeat the one
// recovery the tournament has: a requeued candidate re-asks for the series, and
// if the answer came from this cache it fails identically every time, where
// before the cache each candidate's own fetch could still succeed.
inline constexpr size_t kBarCacheMinBars = 3;

// The cache key: (symbol, IB bar size, IB duration) — the request's identity ON
// THE WIRE, built by net/hist_pacing.h's hist_key() so the pacing gate and this
// cache can never disagree about what "the same request" is.
//
// It includes the DURATION, not just symbol + bar size, because those are
// genuinely different datasets and conflating them is a defect this codebase has
// already shipped once: on 2026-08-10 a sweep labelled "5m 6mo" ran on 1567
// live-warmup bars (a real 6mo request returns ~2966-9517), because candle
// deliveries were routed to a waiting sweep by SYMBOL alone. A cache keyed
// without the span would institutionalise exactly that — serving a warmup series
// to an optimizer asking for six months, every time, silently.
//
// It is the RESOLVED duration, after max_dur_idx clamps the caller's range, not
// the caller's label. Two ranges that clamp to the same duration (1h/"5y" and
// 1h/"max" both send dur="2 Y") are one request as far as IB is concerned: it
// returns the same series for both, and issuing the second inside 15 s is a
// pacing violation. Keying on the caller's label made them two entries, so the
// cache could not answer the duplicate and the gate could only delay it.
struct BarCacheKey {
    std::string symbol, bar_size, duration;
};

class BarCache {
public:
    // Look up a series. Returns nullptr on a miss (absent, or past its TTL).
    // The pointer is valid until the next put()/clear(); copy out of it.
    //
    // `now_ms` must come from a STEADY clock, for the reason HistoryFreshness
    // documents: the VPS re-syncs NTP around the nightly gateway restart, and a
    // wall-clock jump would either resurrect an expired entry or expire a fresh
    // one.
    const std::vector<Candle>* get(const std::string& symbol,
                                   const std::string& bar_size,
                                   const std::string& duration, int64_t now_ms) {
        const auto it = map_.find(key(symbol, bar_size, duration));
        if (it == map_.end()) {
            ++misses_;
            return nullptr;
        }
        if (now_ms - it->second.stamp_ms > kBarCacheTtlMs) {
            candles_ -= it->second.candles.size();
            map_.erase(it);
            ++misses_;
            return nullptr;
        }
        it->second.used_ms = now_ms;
        ++hits_;
        return &it->second.candles;
    }

    // Record a delivered series. An UNUSABLE delivery is not cached: on the
    // ibkr_web route a gateway that has lost an instrument's market-data
    // entitlement answers with no bars at all, and on the TWS route a request
    // IB declines can end the same way — or come back with one or two bars,
    // which is the same nothing wearing a different shape. See kBarCacheMinBars.
    void put(const std::string& symbol, const std::string& bar_size,
             const std::string& duration, std::vector<Candle> candles,
             int64_t now_ms) {
        if (candles.size() < kBarCacheMinBars) return;
        const std::string k = key(symbol, bar_size, duration);
        const auto it = map_.find(k);
        if (it != map_.end()) candles_ -= it->second.candles.size();
        candles_ += candles.size();
        Entry& e = map_[k];
        e.candles = std::move(candles);
        e.stamp_ms = e.used_ms = now_ms;
        evict(now_ms);
    }

    // Session teardown. See kBarCacheTtlMs: bars belong to the session that
    // delivered them.
    void clear() {
        map_.clear();
        candles_ = 0;
    }

    // Observability. Logged by the caller on every lookup so the next
    // investigation can see, in the terminal log, whether the build was
    // re-fetching — the 2026-08-10 profile had to be reconstructed from
    // delivery timestamps because nothing said it directly.
    //
    // These count LOOKUPS, not requests. A request the pacing gate holds back is
    // re-checked against the cache on every io_loop pass, so it can log several
    // misses and then a hit when the in-flight original delivers. That is the
    // truthful reading and it is the useful one: it says the duplicate was
    // waiting rather than being sent.
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    size_t entries() const { return map_.size(); }
    size_t candles() const { return candles_; }

private:
    struct Entry {
        std::vector<Candle> candles;
        int64_t stamp_ms = 0;   // when it was DELIVERED: what the TTL measures
        int64_t used_ms = 0;    // when it was last served: what eviction ranks
    };

    static std::string key(const std::string& symbol, const std::string& bar_size,
                           const std::string& duration) {
        // The same construction as net/hist_pacing.h's hist_key(): unit
        // separator, which appears in no ticker, bar size or duration string, so
        // no two distinct triples can collide into one key.
        return hist_key(symbol, bar_size, duration);
    }

    // Drop expired entries first (they are worthless), then least-recently-USED
    // until both bounds hold. Ranking by last use rather than by insertion keeps
    // the series a tournament is actively re-reading, which is the whole point.
    void evict(int64_t now_ms) {
        for (auto it = map_.begin(); it != map_.end();) {
            if (now_ms - it->second.stamp_ms > kBarCacheTtlMs) {
                candles_ -= it->second.candles.size();
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        // A series larger than the candle ceiling on its own is simply not
        // cached: the loop drops it again on the put that added it. Nothing IB
        // returns comes close (the largest observed is ~9517 bars), and the
        // alternative — honouring one giant entry — would defeat the bound.
        while (!map_.empty() &&
               (map_.size() > kBarCacheMaxEntries || candles_ > kBarCacheMaxCandles)) {
            auto oldest = map_.begin();
            for (auto it = map_.begin(); it != map_.end(); ++it)
                if (it->second.used_ms < oldest->second.used_ms) oldest = it;
            candles_ -= oldest->second.candles.size();
            map_.erase(oldest);
        }
    }

    std::unordered_map<std::string, Entry> map_;
    size_t candles_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
};

} // namespace tt::net
