// AckLatency: the live order-path latency the optimizer feeds into fill sims.
// Median must have ms resolution and stay robust to multi-second tail acks.
#include "doctest.h"

#include "engine/ack_latency.h"

using namespace tt;

TEST_CASE("AckLatency: empty is zero; median has ms resolution") {
    AckLatency a;
    CHECK(a.summary().count == 0);
    CHECK(a.summary().base_ns == 0);
    CHECK(a.summary().jitter_ns == 0);

    for (int i = 0; i < 100; ++i) a.record_ms(68);
    const AckSummary s = a.summary();
    CHECK(s.count == 100);
    CHECK(s.base_ns == 68'000'000);   // p50 of all-68 = 68 ms
    CHECK(s.jitter_ns == 0);          // p90 == p50, no spread
}

TEST_CASE("AckLatency: spread surfaces as jitter (p90 - p50)") {
    AckLatency a;
    for (int ms = 50; ms <= 149; ++ms) a.record_ms(ms);   // 100 samples, one per ms
    const AckSummary s = a.summary();
    CHECK(s.count == 100);
    CHECK(s.base_ns == 100'000'000);    // p50 = 100 ms
    CHECK(s.jitter_ns == 40'000'000);   // p90 - p50 = 140 - 100 ms
}

TEST_CASE("AckLatency: median is robust to multi-second tail outliers") {
    AckLatency a;
    for (int i = 0; i < 100; ++i) a.record_ms(70);
    for (int i = 0; i < 5; ++i) a.record_ms(27'000);   // 27 s hung acks -> overflow bin
    const AckSummary s = a.summary();
    CHECK(s.count == 105);              // outliers counted...
    CHECK(s.base_ns == 70'000'000);     // ...but the median is unmoved
}
