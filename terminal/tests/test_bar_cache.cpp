// The bar cache exists because the 2026-08-10 daily lineup build spent 1501 of
// its 1543 seconds (97.3%) in dead IO wait while the engine did 3398 ms of work
// in total. A tournament's five candidates each re-fetched the SAME symbol's
// SAME series; SSPC 5m/6mo went out four times, two of them 12 seconds apart,
// which is IB's own definition of a pacing violation.
//
// These tests pin the three things the cache has to get right: it must answer a
// repeat of the same request, it must NEVER answer a different one, and it must
// not grow without bound or serve bars from another era.
#include "doctest.h"

#include "net/bar_cache.h"

#include <string>
#include <vector>

using namespace tt::net;

namespace {

std::vector<tt::Candle> series(size_t n, double first_close = 1.0) {
    std::vector<tt::Candle> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back(tt::Candle{static_cast<int64_t>(i), 1, 2, 0.5,
                               first_close + static_cast<double>(i), 100});
    return v;
}

} // namespace

TEST_CASE("an empty cache misses, and a delivered series then hits") {
    BarCache c;
    CHECK(c.get("SSPC", "5m", "6mo", 0) == nullptr);
    CHECK(c.served() == 0);
    CHECK(c.lookups() == 1);
    // A failed lookup on its own is NOT a fetch. It becomes one when the caller
    // actually sends, and only then.
    CHECK(c.fetched() == 0);
    c.record_fetch();
    CHECK(c.fetched() == 1);

    c.put("SSPC", "5m", "6mo", series(9'000), 0);
    const std::vector<tt::Candle>* hit = c.get("SSPC", "5m", "6mo", 1'000);
    REQUIRE(hit != nullptr);
    CHECK(hit->size() == 9'000);
    CHECK(c.served() == 1);
    CHECK(c.entries() == 1);
    CHECK(c.candles() == 9'000);
}

TEST_CASE("the four SSPC fetches collapse to one") {
    // The literal 2026-08-10 sequence: deliveries at 09:39:21, 09:39:33,
    // 09:40:36 and 09:40:41 for one symbol's one series. Only the first is a
    // fetch now; the other three are answered here, and the 12-second gap that
    // broke IB's identical-request rule never reaches the wire.
    BarCache c;
    int64_t t = 0;
    CHECK(c.get("SSPC", "5m", "6mo", t) == nullptr);   // the one real fetch
    c.record_fetch();
    c.put("SSPC", "5m", "6mo", series(2'966), t);
    for (const int64_t at : {12'000, 75'000, 80'000}) {
        const std::vector<tt::Candle>* hit = c.get("SSPC", "5m", "6mo", at);
        REQUIRE(hit != nullptr);
        CHECK(hit->size() == 2'966);
    }
    CHECK(c.served() == 3);
    CHECK(c.fetched() == 1);
    // Four requests in, one fetch out: the counters add up to the work done.
    CHECK(c.served() + c.fetched() == 4);
}

TEST_CASE("the key separates symbol, bar size AND duration") {
    // The key is the request's identity ON THE WIRE — what TwsData actually sends
    // — so these are IB's own strings, not the caller's ("5m", "6mo") labels.
    BarCache c;
    c.put("SOXL", "5 mins", "6 M", series(9'517), 0);
    // The one that matters. A 5m/6mo series and a 5m warmup series are not the
    // same data: on 2026-08-10 a sweep labelled "5m 6mo" optimized on 1567
    // warmup bars because a delivery was matched by symbol alone. A cache keyed
    // without the duration would make that the DESIGNED behaviour.
    CHECK(c.get("SOXL", "5 mins", "1 D", 0) == nullptr);
    CHECK(c.get("SOXL", "1 day", "6 M", 0) == nullptr);
    CHECK(c.get("KORU", "5 mins", "6 M", 0) == nullptr);
    // ...and the exact triple still hits.
    REQUIRE(c.get("SOXL", "5 mins", "6 M", 0) != nullptr);
    // Separate entries, not one overwritten entry.
    c.put("SOXL", "5 mins", "1 D", series(78), 0);
    CHECK(c.entries() == 2);
    REQUIRE(c.get("SOXL", "5 mins", "6 M", 0) != nullptr);
    CHECK(c.get("SOXL", "5 mins", "6 M", 0)->size() == 9'517);
    REQUIRE(c.get("SOXL", "5 mins", "1 D", 0) != nullptr);
    CHECK(c.get("SOXL", "5 mins", "1 D", 0)->size() == 78);
}

TEST_CASE("two caller ranges that clamp to one duration are ONE entry") {
    // The cache is keyed by the same hist_key() the pacing gate uses, so a fetch
    // answers every caller IB could not have told apart. 1h/"5y" and 1h/"max"
    // both send dur="2 Y": before this the second was a cache MISS whose request
    // the gate could only delay by 15 s and then let through, so a duplicate
    // still reached the wire.
    BarCache c;
    const HistWire five_y = hist_wire("1h", "5y");
    const HistWire maximum = hist_wire("1h", "max");
    REQUIRE(five_y.ok());
    REQUIRE(maximum.ok());
    c.put("SPY", five_y.bar, five_y.dur, series(3'400), 0);
    REQUIRE(c.get("SPY", maximum.bar, maximum.dur, 0) != nullptr);
    CHECK(c.get("SPY", maximum.bar, maximum.dur, 0)->size() == 3'400);
    CHECK(c.entries() == 1);
}

TEST_CASE("the key cannot be forged by pasting fields together") {
    // Guards the separator: "AB" + "C" must not collide with "A" + "BC".
    BarCache c;
    c.put("AB", "C", "5m", series(3), 0);
    CHECK(c.get("A", "BC", "5m", 0) == nullptr);
    CHECK(c.get("A", "B", "C5m", 0) == nullptr);
}

TEST_CASE("an entry expires at the TTL and is re-fetched") {
    BarCache c;
    c.put("MUU", "5m", "6mo", series(4'000), 1'000);
    // Inside the window: still served.
    REQUIRE(c.get("MUU", "5m", "6mo", 1'000 + kBarCacheTtlMs) != nullptr);
    // Past it: a miss, and the entry is dropped rather than left to rot.
    CHECK(c.get("MUU", "5m", "6mo", 1'001 + kBarCacheTtlMs) == nullptr);
    CHECK(c.entries() == 0);
    CHECK(c.candles() == 0);
}

TEST_CASE("the TTL outlives a tournament but not a re-optimize cycle") {
    // Both bounds the TTL was chosen against, as a test rather than a comment:
    // it must cover one symbol's five candidates back to back, and it must NOT
    // reach the autopilot's 30-minute cadence, or a re-optimization would score
    // the same bars it scored last time and report success.
    CHECK(kBarCacheTtlMs >= 60'000);
    CHECK(kBarCacheTtlMs < 30 * 60 * 1000);
}

TEST_CASE("an empty delivery is not cached") {
    // A gateway that has lost an instrument's market-data entitlement answers
    // with no bars at all. Caching that would serve five minutes of confident
    // nothing to every consumer that asks.
    BarCache c;
    c.put("DEAD", "5m", "6mo", {}, 0);
    CHECK(c.entries() == 0);
    CHECK(c.get("DEAD", "5m", "6mo", 0) == nullptr);
}

TEST_CASE("a delivery too thin to optimize on is not cached either") {
    // Guarding only against EMPTY was not enough. App rejects a series under 3
    // bars in both consumers ("not enough data"), so a 1- or 2-bar answer — a
    // halted or newly-listed pick, or a partial answer during a data-farm hiccup
    // — is nothing usable wearing a different shape. Cached, it is replayed for
    // five minutes, and it silently defeats the tournament's one recovery: a
    // requeued candidate re-asks for the series, is answered from this entry, and
    // fails identically. Before the cache existed each candidate's own fetch
    // could still succeed, so caching this would be a REGRESSION.
    BarCache c;
    for (size_t n = 0; n < kBarCacheMinBars; ++n) {
        c.put("HALTED", "5 mins", "6 M", series(n), 0);
        CHECK(c.entries() == 0);
        CHECK(c.get("HALTED", "5 mins", "6 M", 0) == nullptr);
    }
    // At the threshold it caches normally: the cut-off is the app's own.
    c.put("HALTED", "5 mins", "6 M", series(kBarCacheMinBars), 0);
    CHECK(c.entries() == 1);
    REQUIRE(c.get("HALTED", "5 mins", "6 M", 0) != nullptr);
    // ...and it is the same threshold App::pump_sweep / start_pending_backtest
    // use, so the cache can never hold a series they would refuse.
    CHECK(kBarCacheMinBars == 3);
}

TEST_CASE("a re-delivery replaces the entry without double-counting its bars") {
    BarCache c;
    c.put("SNDQ", "5m", "6mo", series(3'000), 0);
    CHECK(c.candles() == 3'000);
    c.put("SNDQ", "5m", "6mo", series(3'100), 1'000);
    CHECK(c.entries() == 1);
    CHECK(c.candles() == 3'100);
    REQUIRE(c.get("SNDQ", "5m", "6mo", 1'000) != nullptr);
    CHECK(c.get("SNDQ", "5m", "6mo", 1'000)->size() == 3'100);
}

TEST_CASE("a refresh restarts the TTL from the new delivery") {
    BarCache c;
    c.put("SNDQ", "5m", "6mo", series(10), 0);
    c.put("SNDQ", "5m", "6mo", series(10), kBarCacheTtlMs - 1);
    // Would be long expired if the stamp had stayed at the FIRST delivery.
    CHECK(c.get("SNDQ", "5m", "6mo", 2 * kBarCacheTtlMs - 2) != nullptr);
}

TEST_CASE("the entry count is bounded") {
    BarCache c;
    // The ranking pass fetches one tiny 1d series per scan hit; a long-running
    // process cycles through hundreds of symbols this way.
    for (size_t i = 0; i < kBarCacheMaxEntries + 40; ++i)
        c.put("SYM" + std::to_string(i), "1d", "1mo", series(21), 0);
    CHECK(c.entries() <= kBarCacheMaxEntries);
}

TEST_CASE("the candle count is bounded") {
    BarCache c;
    // Full-size 5m/6mo series, enough of them to blow the memory ceiling.
    const size_t big = 9'500;
    for (size_t i = 0; i < (kBarCacheMaxCandles / big) + 10; ++i)
        c.put("SYM" + std::to_string(i), "5m", "6mo", series(big), 0);
    CHECK(c.candles() <= kBarCacheMaxCandles);
    CHECK(c.entries() * big == c.candles());   // the accounting stayed honest
}

TEST_CASE("eviction drops the least recently USED, not the oldest fetched") {
    // The series a tournament is actively re-reading is the one worth keeping —
    // that is the entire point of the cache.
    BarCache c;
    const size_t big = 9'500;
    const size_t room = kBarCacheMaxCandles / big;
    for (size_t i = 0; i < room; ++i)
        c.put("SYM" + std::to_string(i), "5m", "6mo", series(big), 0);
    REQUIRE(c.entries() == room);
    // Touch the OLDEST entry so it is the most recently used.
    REQUIRE(c.get("SYM0", "5m", "6mo", 1'000) != nullptr);
    // Now force one eviction.
    c.put("NEW", "5m", "6mo", series(big), 2'000);
    CHECK(c.entries() == room);
    CHECK(c.get("SYM0", "5m", "6mo", 3'000) != nullptr);   // survived
    CHECK(c.get("NEW", "5m", "6mo", 3'000) != nullptr);    // the new arrival
    // ...and the one that went was one of the untouched ones. (WHICH untouched
    // one is unspecified: they all carry the same last-used stamp, and the map
    // is unordered. Pinning a particular victim would be testing the hash.)
    size_t survivors = 0;
    for (size_t i = 1; i < room; ++i)
        if (c.get("SYM" + std::to_string(i), "5m", "6mo", 3'000)) ++survivors;
    CHECK(survivors == room - 2);
}

TEST_CASE("expired entries are evicted ahead of live ones") {
    BarCache c;
    c.put("STALE", "5m", "6mo", series(100), 0);
    c.put("FRESH", "5m", "6mo", series(100), kBarCacheTtlMs);
    // The put below runs eviction at a time where STALE is past its TTL.
    c.put("NEWEST", "5m", "6mo", series(100), kBarCacheTtlMs + 1);
    CHECK(c.entries() == 2);
    CHECK(c.get("STALE", "5m", "6mo", kBarCacheTtlMs + 1) == nullptr);
    CHECK(c.get("FRESH", "5m", "6mo", kBarCacheTtlMs + 1) != nullptr);
}

// ------------------------------------------------------------------ accounting
//
// The counters shipped broken and the numbers were read as evidence. These pin
// the ONE property that makes them mean anything: a served() is a fetch that did
// not happen, and served() + fetched() is the number of REQUESTS resolved.

TEST_CASE("a pacing-held request counts ONCE, not once per io_loop pass") {
    // The 2026-08-11 defect in miniature, and the reason the counter moved out
    // of get(). TwsData::Io::pump_requests re-offers every queued request to the
    // cache on EVERY io_loop pass, and that loop spins on inbound quote
    // messages rather than on a timer — so one request the pacing gate holds for
    // a few seconds consults the cache dozens of times. Counting the failed
    // lookup made that ONE request read as dozens of cache misses, which is how
    // the dry run came to report
    //
    //     cache_hits=24 cache_misses=967 hist_requests=36
    //
    // 967 against 36 was not a contradiction to be explained away; it was the
    // gate's churn wearing the cache's label.
    BarCache c;
    const std::string sym = "TQQQ", bar = "5 mins", dur = "6 M";

    // Candidate 1 asks. Nothing cached, so it goes on the wire.
    int64_t t = 0;
    CHECK(c.get(sym, bar, dur, t) == nullptr);
    c.record_fetch();

    // Candidates 2-5 are queued behind it. Until the delivery lands they are
    // held by the gate and re-offered on every pass — each re-offer is another
    // get() that finds nothing.
    constexpr int kDuplicates = 4;
    constexpr int kPassesInFlight = 40;
    for (int p = 0; p < kPassesInFlight; ++p)
        for (int d = 0; d < kDuplicates; ++d)
            CHECK(c.get(sym, bar, dur, ++t) == nullptr);

    // The answer arrives; every waiting candidate is served from it.
    c.put(sym, bar, dur, series(9'517), ++t);
    for (int d = 0; d < kDuplicates; ++d) REQUIRE(c.get(sym, bar, dur, ++t) != nullptr);

    // Five requests. ONE fetch. That is the whole claim.
    CHECK(c.fetched() == 1);
    CHECK(c.served() == kDuplicates);
    CHECK(c.served() + c.fetched() == 1 + kDuplicates);

    // And the number that used to be called "misses", correctly labelled: it
    // measures how hard the gate was deferring, not how badly the cache missed.
    CHECK(c.lookups() == 1 + kDuplicates * kPassesInFlight + kDuplicates);
    CHECK(c.lookups() > 10 * (c.served() + c.fetched()));
}

TEST_CASE("a request that misses and is later served is served, not both") {
    // The reason the fetch is counted at the SEND and not at the failed lookup.
    // Counting a miss the first time a request consults the cache would still be
    // wrong for the commonest case in a tournament: the duplicate misses while
    // the original is in flight, then hits. It would score one miss AND one hit
    // for a single request, and hits + misses would exceed the requests made.
    BarCache c;
    CHECK(c.get("SOXL", "5 mins", "6 M", 0) == nullptr);   // in flight, held
    CHECK(c.get("SOXL", "5 mins", "6 M", 1) == nullptr);   // still held
    c.put("SOXL", "5 mins", "6 M", series(9'000), 2);
    REQUIRE(c.get("SOXL", "5 mins", "6 M", 3) != nullptr);
    CHECK(c.served() == 1);
    CHECK(c.fetched() == 0);   // this request never reached the wire
    CHECK(c.served() + c.fetched() == 1);
}

TEST_CASE("a cold build is all fetches and a warm one is all serves") {
    // The two ends of the scale, so the ratio is anchored: nothing cached means
    // fetched() == requests and a 0% hit rate; everything cached means
    // served() == requests and 100%. Neither is expressible if a "miss" can be
    // logged more than once per request.
    BarCache cold;
    for (int i = 0; i < 30; ++i) {   // the lineup's 30-symbol ranking pass
        const std::string s = "SYM" + std::to_string(i);
        CHECK(cold.get(s, "1 day", "1 M", 0) == nullptr);
        cold.record_fetch();
        cold.put(s, "1 day", "1 M", series(21), 0);
    }
    CHECK(cold.fetched() == 30);
    CHECK(cold.served() == 0);

    // Same 30 symbols again, inside the TTL: not one request reaches IB.
    for (int i = 0; i < 30; ++i)
        REQUIRE(cold.get("SYM" + std::to_string(i), "1 day", "1 M", 1'000) != nullptr);
    CHECK(cold.fetched() == 30);
    CHECK(cold.served() == 30);
}

TEST_CASE("an expired entry is a fetch again, not a free serve") {
    // A hit must mean a fetch AVOIDED. An entry past its TTL avoids nothing —
    // the caller has to go to IB — so it must not be counted as served.
    BarCache c;
    c.put("MUU", "5m", "6mo", series(4'000), 0);
    REQUIRE(c.get("MUU", "5m", "6mo", kBarCacheTtlMs) != nullptr);
    CHECK(c.served() == 1);
    CHECK(c.get("MUU", "5m", "6mo", kBarCacheTtlMs + 1) == nullptr);
    CHECK(c.served() == 1);   // unchanged: nothing was avoided
    c.record_fetch();
    CHECK(c.fetched() == 1);
}

TEST_CASE("clear() drops the bars but not the record of what the process did") {
    // drop_connection() calls clear() on every data-session teardown, and the
    // dry run brackets these counters as a delta across the build. Resetting
    // them there would silently subtract every request made before a mid-build
    // reconnect — and a reconnect is exactly when the numbers matter most.
    BarCache c;
    CHECK(c.get("SOXS", "5m", "6mo", 0) == nullptr);
    c.record_fetch();
    c.put("SOXS", "5m", "6mo", series(5'000), 0);
    REQUIRE(c.get("SOXS", "5m", "6mo", 0) != nullptr);
    c.clear();
    CHECK(c.entries() == 0);
    CHECK(c.served() == 1);
    CHECK(c.fetched() == 1);
    CHECK(c.lookups() == 2);
}

TEST_CASE("clear() drops everything, because bars belong to a session") {
    // Called from TwsData::Io::drop_connection. The usual reason we get there is
    // that history stopped being answered; replaying that session's last answers
    // across the reconnect would hide the very failure it exists to clear.
    BarCache c;
    c.put("SOXS", "5m", "6mo", series(5'000), 0);
    c.clear();
    CHECK(c.entries() == 0);
    CHECK(c.candles() == 0);
    CHECK(c.get("SOXS", "5m", "6mo", 0) == nullptr);
}
