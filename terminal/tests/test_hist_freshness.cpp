// Bar-freshness tracking. On 2026-08-07 the data client failed every
// historical-bar request on a 30-minute cycle from ~10:31; SOXS/AAOX/SNDQ
// candles went 4.5-5.2 hours stale while their strategies kept trading, and
// /diag reported data.connected true and a flat oldest_history_age_ms the whole
// time. These cases pin the two properties that make this metric able to see
// that, which the pending-request metric structurally cannot:
//   - it is driven only by SUCCESSFUL deliveries, and
//   - a symbol that has never been answered is stale, not absent.
#include "doctest.h"

#include "net/hist_freshness.h"

using namespace tt::net;

// Ages are in ms off a steady clock; the tests just hand in their own "now".
static constexpr int64_t kMin = 60'000;

TEST_CASE("freshness: a symbol with no delivery has no age") {
    HistoryFreshness f;
    CHECK(f.age_ms("SOXS", "5m", 1'000) == -1);
}

TEST_CASE("freshness: age is measured from the last delivery, and only grows") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    CHECK(f.age_ms("SOXS", "5m", 0) == 0);
    CHECK(f.age_ms("SOXS", "5m", 90 * kMin) == 90 * kMin);
    // The next successful fetch resets it — that is the whole signal.
    f.record("SOXS", "5m", 90 * kMin);
    CHECK(f.age_ms("SOXS", "5m", 91 * kMin) == kMin);
}

TEST_CASE("freshness: a different interval does not mark a symbol fresh") {
    // The daily lineup builder fetches "1d" bars for every candidate. Keying on
    // the symbol alone would let that hide a five-hour-old 5m series, which is
    // the series the engine seeds and the optimizer scores.
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    f.record("SOXS", "1d", 300 * kMin);
    CHECK(f.age_ms("SOXS", "5m", 300 * kMin) == 300 * kMin);
    CHECK(f.age_ms("SOXS", "1d", 300 * kMin) == 0);
}

TEST_CASE("freshness: worst_age_ms picks the oldest and ignores the unseen") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    f.record("AAOX", "5m", 30 * kMin);
    const std::vector<std::string> syms{"SOXS", "AAOX", "NEVER"};
    CHECK(f.worst_age_ms(syms, "5m", 60 * kMin) == 60 * kMin);   // SOXS
    CHECK(f.worst_age_ms({}, "5m", 60 * kMin) == -1);
    CHECK(f.worst_age_ms({"NEVER"}, "5m", 60 * kMin) == -1);
}

// ---- grace period ----------------------------------------------------------

TEST_CASE("grace: the production 30-minute cadence gets 90 minutes") {
    // Live bars are only re-fetched by the autopilot's optimize cycle, which is
    // serialized across the lineup, so one slipped slot is normal. 3x cadence.
    CHECK(bar_stale_grace_ms(30) == 90 * kMin);
}

TEST_CASE("grace: a healthy 30-minute rhythm can never trip the watchdog") {
    // The property that matters: two consecutive missed refreshes still sit
    // inside the grace, so only a genuine stall pages.
    const int64_t grace = bar_stale_grace_ms(30);
    CHECK(grace > 2 * 30 * kMin);
    // ...and the 2026-08-07 outage is well outside it. Last good SOXS refresh
    // 09:17; still stale at 14:01 (284 min). It would have paged at 10:47.
    CHECK(284 * kMin > grace);
}

TEST_CASE("grace: a short autopilot interval falls back to the floor") {
    // A tournament legitimately takes minutes; 3 x 5 min would cry wolf.
    CHECK(bar_stale_grace_ms(5) == kBarStaleGraceFloorMs);
    CHECK(bar_stale_grace_ms(0) == kBarStaleGraceFloorMs);
    CHECK(bar_stale_grace_ms(-1) == kBarStaleGraceFloorMs);
    // The floor applies below 15 min and the cadence takes over above it.
    CHECK(bar_stale_grace_ms(15) == kBarStaleGraceFloorMs);
    CHECK(bar_stale_grace_ms(20) == 60 * kMin);
}

// ---- what the watchdog actually asks ---------------------------------------

TEST_CASE("stale: nothing is reported while every symbol refreshes") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    f.record("AAOX", "5m", 0);
    const std::vector<std::string> syms{"SOXS", "AAOX"};
    const int64_t grace = bar_stale_grace_ms(30);
    // 89 minutes old on a 90-minute grace: still quiet.
    CHECK(f.stale(syms, "5m", 89 * kMin, grace, 89 * kMin).empty());
}

TEST_CASE("stale: the 2026-08-07 lineup, replayed") {
    // Minutes from 09:00. Last good refresh: SOXS 09:17, AAOX 09:47,
    // SNDQ 10:01; SPCH kept refreshing. "Now" is the 14:01 log line.
    HistoryFreshness f;
    f.record("SOXS", "5m", 17 * kMin);
    f.record("AAOX", "5m", 47 * kMin);
    f.record("SNDQ", "5m", 61 * kMin);
    f.record("SPCH", "5m", 295 * kMin);
    const std::vector<std::string> syms{"SOXS", "AAOX", "SNDQ", "SPCH"};
    const auto out = f.stale(syms, "5m", 301 * kMin, bar_stale_grace_ms(30),
                             301 * kMin);
    REQUIRE(out.size() == 3);
    CHECK(out[0].symbol == "SOXS");   // worst first, so the alert leads with it
    CHECK(out[1].symbol == "AAOX");
    CHECK(out[2].symbol == "SNDQ");
    CHECK(out[0].age_ms == 284 * kMin);
    CHECK(out[0].ever);
}

TEST_CASE("stale: a symbol that has NEVER been answered is aged from the session") {
    // The failure can start before a symbol's first refresh lands, so "no
    // delivery ever" must not read as healthy. It also means the grace doubles
    // as a settle-in window: nothing pages in the first 90 minutes of a session.
    HistoryFreshness f;
    const std::vector<std::string> syms{"SOXS"};
    const int64_t grace = bar_stale_grace_ms(30);
    CHECK(f.stale(syms, "5m", 500 * kMin, grace, 89 * kMin).empty());
    const auto out = f.stale(syms, "5m", 500 * kMin, grace, 91 * kMin);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].ever);
    CHECK(out[0].age_ms == 91 * kMin);
}

TEST_CASE("stale: a symbol outside the watched set is never reported") {
    // Only autopilot-armed symbols have a refresh cadence; an unarmed one is
    // stale forever by design and would page every session.
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    CHECK(f.stale({}, "5m", 500 * kMin, bar_stale_grace_ms(30), 500 * kMin)
              .empty());
}

TEST_CASE("stale: a fresh delivery clears the whole condition") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    const std::vector<std::string> syms{"SOXS"};
    const int64_t grace = bar_stale_grace_ms(30);
    CHECK(f.stale(syms, "5m", 200 * kMin, grace, 200 * kMin).size() == 1);
    f.record("SOXS", "5m", 200 * kMin);
    CHECK(f.stale(syms, "5m", 200 * kMin, grace, 200 * kMin).empty());
}
