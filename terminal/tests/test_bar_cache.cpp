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
    CHECK(c.misses() == 1);
    CHECK(c.hits() == 0);

    c.put("SSPC", "5m", "6mo", series(9'000), 0);
    const std::vector<tt::Candle>* hit = c.get("SSPC", "5m", "6mo", 1'000);
    REQUIRE(hit != nullptr);
    CHECK(hit->size() == 9'000);
    CHECK(c.hits() == 1);
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
    c.put("SSPC", "5m", "6mo", series(2'966), t);
    for (const int64_t at : {12'000, 75'000, 80'000}) {
        const std::vector<tt::Candle>* hit = c.get("SSPC", "5m", "6mo", at);
        REQUIRE(hit != nullptr);
        CHECK(hit->size() == 2'966);
    }
    CHECK(c.hits() == 3);
    CHECK(c.misses() == 1);
}

TEST_CASE("the key separates symbol, interval AND range") {
    BarCache c;
    c.put("SOXL", "5m", "6mo", series(9'517), 0);
    // The one that matters. A 5m/6mo series and a 5m warmup series are not the
    // same data: on 2026-08-10 a sweep labelled "5m 6mo" optimized on 1567
    // warmup bars because a delivery was matched by symbol alone. A cache keyed
    // without the range would make that the DESIGNED behaviour.
    CHECK(c.get("SOXL", "5m", "1d", 0) == nullptr);
    CHECK(c.get("SOXL", "1d", "6mo", 0) == nullptr);
    CHECK(c.get("KORU", "5m", "6mo", 0) == nullptr);
    // ...and the exact triple still hits.
    REQUIRE(c.get("SOXL", "5m", "6mo", 0) != nullptr);
    // Separate entries, not one overwritten entry.
    c.put("SOXL", "5m", "1d", series(78), 0);
    CHECK(c.entries() == 2);
    REQUIRE(c.get("SOXL", "5m", "6mo", 0) != nullptr);
    CHECK(c.get("SOXL", "5m", "6mo", 0)->size() == 9'517);
    REQUIRE(c.get("SOXL", "5m", "1d", 0) != nullptr);
    CHECK(c.get("SOXL", "5m", "1d", 0)->size() == 78);
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
