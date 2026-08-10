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

// The production shape: every symbol on the 30-minute autopilot cadence.
static std::vector<WatchedSymbol> every30(const std::vector<std::string>& syms) {
    std::vector<WatchedSymbol> out;
    for (const std::string& s : syms) out.push_back({s, 30});
    return out;
}

TEST_CASE("stale: nothing is reported while every symbol refreshes") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    f.record("AAOX", "5m", 0);
    const auto syms = every30({"SOXS", "AAOX"});
    // 89 minutes old on a 90-minute grace: still quiet.
    CHECK(f.stale(syms, "5m", 89 * kMin, 89 * kMin).empty());
}

TEST_CASE("stale: the 2026-08-07 lineup, replayed") {
    // Minutes from 09:00. Last good refresh: SOXS 09:17, AAOX 09:47,
    // SNDQ 10:01; SPCH kept refreshing. "Now" is the 14:01 log line.
    HistoryFreshness f;
    f.record("SOXS", "5m", 17 * kMin);
    f.record("AAOX", "5m", 47 * kMin);
    f.record("SNDQ", "5m", 61 * kMin);
    f.record("SPCH", "5m", 295 * kMin);
    const auto syms = every30({"SOXS", "AAOX", "SNDQ", "SPCH"});
    const auto out = f.stale(syms, "5m", 301 * kMin, 301 * kMin);
    REQUIRE(out.size() == 3);
    CHECK(out[0].symbol == "SOXS");   // worst first, so the alert leads with it
    CHECK(out[1].symbol == "AAOX");
    CHECK(out[2].symbol == "SNDQ");
    CHECK(out[0].age_ms == 284 * kMin);
    CHECK(out[0].ever);
}

TEST_CASE("stale: a slow symbol does not raise the bar for a fast one") {
    // 0.12.0 took ONE grace off the slowest armed symbol and applied it to the
    // whole lineup. Put a 240-minute re-optimize cadence next to the production
    // 30-minute one and that grace becomes 12 hours for everybody — so replay
    // 2026-08-07 into it and SOXS, 284 minutes stale, sits comfortably inside
    // it. The outage this watchdog was written for would page nobody.
    HistoryFreshness f;
    f.record("SOXS", "5m", 17 * kMin);   // last good refresh 09:17
    f.record("SNDQ", "5m", 17 * kMin);   // same age, but 8x the cadence
    const std::vector<WatchedSymbol> syms{{"SOXS", 30}, {"SNDQ", 240}};
    const auto out = f.stale(syms, "5m", 301 * kMin, 301 * kMin);
    REQUIRE(out.size() == 1);
    CHECK(out[0].symbol == "SOXS");
    CHECK(out[0].age_ms == 284 * kMin);
    // ...and the slow one is judged by its own cadence, not silenced forever.
    CHECK(f.stale(syms, "5m", 800 * kMin, 800 * kMin).size() == 2);
}

TEST_CASE("stale: a symbol that has NEVER been answered is aged from the session") {
    // The failure can start before a symbol's first refresh lands, so "no
    // delivery ever" must not read as healthy. It also means the grace doubles
    // as a settle-in window: nothing pages in the first 90 minutes of a session.
    HistoryFreshness f;
    const auto syms = every30({"SOXS"});
    CHECK(f.stale(syms, "5m", 500 * kMin, 89 * kMin).empty());
    const auto out = f.stale(syms, "5m", 500 * kMin, 91 * kMin);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].ever);
    CHECK(out[0].age_ms == 91 * kMin);
}

TEST_CASE("stale: a symbol outside the watched set is never reported") {
    // Only a symbol on an autopilot TIMER has a refresh cadence. One that is
    // unarmed — or armed on the "Drawdown" trigger alone, which pump_autopilot
    // never runs a timed cycle for — is stale forever by design and would page
    // every session. pump_history_watchdog keeps both out of this list.
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    CHECK(f.stale({}, "5m", 500 * kMin, 500 * kMin).empty());
}

TEST_CASE("stale: a fresh delivery clears the whole condition") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    const auto syms = every30({"SOXS"});
    CHECK(f.stale(syms, "5m", 200 * kMin, 200 * kMin).size() == 1);
    f.record("SOXS", "5m", 200 * kMin);
    CHECK(f.stale(syms, "5m", 200 * kMin, 200 * kMin).empty());
}

TEST_CASE("stale: a stopped session's deliveries do not follow it into the next") {
    // The terminal stays up across the 15:55 scheduled stop and the 09:25
    // auto-start, and an answered symbol is aged ABSOLUTELY — the settle-in
    // window only ever covers a symbol that has never been answered. Without
    // the reset, yesterday's 15:32 delivery pages ~17 hours of staleness on the
    // first frame of a healthy morning, twice, every day.
    HistoryFreshness f;
    f.record("SOXS", "5m", 392 * kMin);   // 15:32, counting minutes from 09:00
    const auto syms = every30({"SOXS"});
    const int64_t next_open = 1'465 * kMin;   // 09:25 the next morning
    CHECK(f.stale(syms, "5m", next_open, 1 * kMin).size() == 1);
    f.clear();
    CHECK(f.age_ms("SOXS", "5m", next_open) == -1);
    // Now it reads as never-answered and the grace acts as the settle-in window.
    CHECK(f.stale(syms, "5m", next_open, 1 * kMin).empty());
    CHECK(f.stale(syms, "5m", next_open, 91 * kMin).size() == 1);
}

// ---- the page text ---------------------------------------------------------
// The 2026-08-10 stall killed history for four symbols while two others were
// served perfectly by the SAME socket, session and farms. The old page ended
// "(data socket reports CONNECTED - check IB Gateway)", which sends the operator
// to restart something the evidence says is healthy — and a forced re-login can
// hit IBKR's maintenance window or, repeated, lock the account.
namespace {
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("refreshing: only a recent delivery of a symbol's OWN counts") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 100 * kMin);
    const std::vector<WatchedSymbol> syms{{"SOXS", 30}, {"NEVER", 30}};
    // Inside the evidence window: the session is demonstrably serving history.
    auto out = f.refreshing(syms, "5m", 100 * kMin + kRefreshEvidenceMs);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "SOXS");
    // Past it, the same symbol stops being evidence of anything...
    CHECK(f.refreshing(syms, "5m", 100 * kMin + kRefreshEvidenceMs + 1).empty());
    // ...and a symbol that has never delivered can never become evidence.
    f.record("NEVER", "1d", 100 * kMin);   // a lineup 1d pull is not its 5m series
    CHECK(f.refreshing(syms, "5m", 101 * kMin).size() == 1);
}

TEST_CASE("refreshing: a slow-cadence symbol inside its grace is not evidence") {
    // THE 0.15.0 DEFECT. hist_stall_alert derived its healthy set as "watched
    // minus stale", i.e. "has not crossed its OWN grace period". Put a 240-min
    // re-optimize cadence (720-min grace) next to the production 30-min one and
    // a total gateway outage at 10:00 leaves SOXS stale at 11:31 while MUU,
    // silent just as long, is still inside its grace — so the page named MUU as
    // "still refreshing on the same session" and told the operator NOT to
    // restart the gateway that had in fact died.
    HistoryFreshness f;
    f.record("SOXS", "5m", 60 * kMin);   // both last delivered at 10:00
    f.record("MUU", "5m", 60 * kMin);
    const std::vector<WatchedSymbol> syms{{"SOXS", 30}, {"MUU", 240}};
    const int64_t now = 151 * kMin;      // 11:31
    const auto stale = f.stale(syms, "5m", now, now);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].symbol == "SOXS");    // MUU is not stale: 91m of a 720m grace
    CHECK(f.refreshing(syms, "5m", now).empty());   // ...but it is not refreshing
    const std::string msg =
        hist_stall_alert("SOXS 91m", stale, f.refreshing(syms, "5m", now), true);
    CHECK(has(msg, "NO watched symbol is refreshing"));
    CHECK_FALSE(has(msg, "do NOT restart the gateway"));
    CHECK_FALSE(has(msg, "MUU"));
}

TEST_CASE("refreshing: a never-answered symbol cannot clear the gateway") {
    // Same shape at session start, where stale() ages a never-answered symbol
    // from session_ms: at 47 minutes the 15-min symbol is past its floor grace
    // and the 60-min one is not, so "watched minus stale" nominated a symbol
    // that had delivered nothing at all.
    HistoryFreshness f;
    const std::vector<WatchedSymbol> syms{{"SOXS", 15}, {"SNDQ", 60}};
    const int64_t now = 47 * kMin;
    const auto stale = f.stale(syms, "5m", now, now);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].symbol == "SOXS");
    CHECK_FALSE(stale[0].ever);
    CHECK(f.refreshing(syms, "5m", now).empty());
}

TEST_CASE("alert: a stall beside healthy symbols is not blamed on the gateway") {
    const std::vector<StaleBars> stale{{"SOXS", 284 * kMin, true},
                                       {"MUU", 60 * kMin, false}};
    const std::string msg =
        hist_stall_alert("SOXS 284m, MUU never", stale,
                         {"AAOX", "SNDQ"}, true);
    CHECK(has(msg, "SOXS 284m, MUU never"));
    // The contrast that IS the diagnosis: same session, still refreshing.
    CHECK(has(msg, "AAOX, SNDQ"));
    CHECK(has(msg, "app-side history stall, not the gateway"));
    CHECK(has(msg, "do NOT restart the gateway"));
    CHECK_FALSE(has(msg, "check IB Gateway"));
    CHECK(has(msg, "2 traded symbol(s)"));
}

TEST_CASE("alert: with nothing refreshing the gateway is not cleared either") {
    // Every watched symbol is stale, so the upstream is still a live suspect —
    // but the operator is pointed at the log first, not at a blind restart.
    const std::vector<StaleBars> stale{{"SOXS", 284 * kMin, true}};
    const std::string msg = hist_stall_alert("SOXS 284m", stale, {}, true);
    CHECK(has(msg, "NO watched symbol is refreshing"));
    CHECK(has(msg, "before touching the gateway"));
    CHECK_FALSE(has(msg, "check IB Gateway"));
    CHECK_FALSE(has(msg, "not the gateway:"));   // no verdict without evidence
}

TEST_CASE("alert: a dropped socket says the reconnect path already owns it") {
    const std::vector<StaleBars> stale{{"SOXS", 284 * kMin, true}};
    const std::string msg =
        hist_stall_alert("SOXS 284m", stale, {"AAOX"}, false);
    CHECK(has(msg, "data socket disconnected"));
    CHECK(has(msg, "reconnect path is already on it"));
    CHECK_FALSE(has(msg, "gateway"));   // nothing for the operator to restart
}
