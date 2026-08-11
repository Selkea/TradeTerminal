// Bar-freshness tracking. On 2026-08-07 the data client failed every
// historical-bar request on a 30-minute cycle from ~10:31; SOXS/AAOX/SNDQ
// candles went 4.5-5.2 hours stale while their strategies kept trading, and
// /diag reported data.connected true and a flat oldest_history_age_ms the whole
// time. These cases pin the two properties that make this metric able to see
// that, which the pending-request metric structurally cannot:
//   - it is driven only by SUCCESSFUL deliveries, and
//   - a symbol that has never been answered is stale, not absent.
#include "doctest.h"

#include "market_calendar.h"   // the RTH gate the watchdog is armed by
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
    // auto-start. Without the reset, yesterday's 15:32 delivery is ~17 hours old
    // on the first frame of a healthy morning — and while armed_ms now caps that
    // (below), clear() is what keeps /diag's age and refreshing()'s evidence
    // honest, so both properties are pinned.
    HistoryFreshness f;
    f.record("SOXS", "5m", 392 * kMin);   // 15:32, counting minutes from 09:00
    const auto syms = every30({"SOXS"});
    const int64_t next_open = 1'465 * kMin;   // 09:25 the next morning
    // A fresh session is one minute old, so nothing is judged yet either way.
    CHECK(f.stale(syms, "5m", next_open, 1 * kMin).empty());
    // ...but the carried-over delivery is still visible as an age, and would
    // still be offered as proof the session is serving history if it were recent.
    CHECK(f.age_ms("SOXS", "5m", next_open) == 1'073 * kMin);
    f.clear();
    CHECK(f.age_ms("SOXS", "5m", next_open) == -1);
    // Now it reads as never-answered and the grace acts as the settle-in window.
    CHECK(f.stale(syms, "5m", next_open, 1 * kMin).empty());
    CHECK(f.stale(syms, "5m", next_open, 91 * kMin).size() == 1);
}

// ---- the RTH gate ----------------------------------------------------------
// 2026-08-11: a live session was left running past the 15:55 auto-stop and this
// watchdog paged Critical at 18:40 and 19:10 ("strategies are trading on stale
// candles") with the market shut since 16:00 and the book flat, and would have
// gone on every 30 minutes until morning. The gate is armed_ms: the shorter of
// the session's age and the time the market has been open.

TEST_CASE("armed: the shorter of the two windows wins") {
    CHECK(hist_armed_ms(600 * kMin, 45 * kMin) == 45 * kMin);   // long session, fresh open
    CHECK(hist_armed_ms(1 * kMin, 300 * kMin) == 1 * kMin);     // just started mid-session
    CHECK(hist_armed_ms(600 * kMin, 0) == 0);                   // market shut
    CHECK(hist_armed_ms(0, 300 * kMin) == 0);
}

TEST_CASE("armed: outside market hours nothing is overdue, however old") {
    // The exact 19:10 page: MUU 120m, SOXS/KORU/SOXL 119m, all four genuinely
    // not refreshing, all four meaningless — every order the engine emits is
    // outside_rth = 0, so a strategy acting on a stale candle cannot fill.
    HistoryFreshness f;
    f.record("MUU", "5m", 0);
    f.record("SOXS", "5m", 1 * kMin);
    const auto syms = every30({"MUU", "SOXS", "KORU", "SOXL"});
    const int64_t now = 120 * kMin;
    // Armed (mid-session), this is exactly what the operator was paged about.
    CHECK(f.stale(syms, "5m", now, 400 * kMin).size() == 4);
    // Closed: the same evidence, judged against a shut market.
    CHECK(f.stale(syms, "5m", now, 0).empty());
    // ...and it stays empty as the night wears on, which is the whole point:
    // the 30-minute re-alert cannot find anything to re-page.
    CHECK(f.stale(syms, "5m", 900 * kMin, 0).empty());
}

TEST_CASE("armed: the open boundary does not page an overnight-stale symbol") {
    // The case the gate is most likely to get wrong. A session that was never
    // stopped is still holding yesterday's deliveries, so at 09:30:01 a symbol
    // last served at 17:00 is sixteen hours old — and the morning's first
    // autopilot cycle has not run yet. Minutes from yesterday 09:00.
    HistoryFreshness f;
    f.record("MUU", "5m", 480 * kMin);   // 17:00 yesterday
    const auto syms = every30({"MUU"});   // production cadence: a 90-minute grace
    // Minutes from yesterday 09:00, so TODAY 09:30 is t = 1470.
    auto today = [](int hh, int mm) {
        return static_cast<int64_t>(1440 + hh * 60 + mm - 540) * kMin;
    };
    auto armed_at = [&](int hh, int mm) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 7; tm.tm_mday = 12;   // Wednesday
        tm.tm_wday = tt::weekday_of(2026, 8, 12);
        tm.tm_hour = hh; tm.tm_min = mm;
        // Live since yesterday 09:25 (t = 25) and never stopped.
        return hist_armed_ms(today(hh, mm) - 25 * kMin, tt::rth_open_elapsed_ms(tm));
    };
    auto at = [&](int hh, int mm) {
        return f.stale(syms, "5m", today(hh, mm), armed_at(hh, mm));
    };
    CHECK(at(9, 30).empty());    // the instant the gate opens: 16.5 h "stale"
    CHECK(at(10, 59).empty());   // 89 minutes of open market, inside the grace
    // 91 minutes of open market with nothing delivered IS the failure, so it
    // pages then — and reports the TRUE age, not the 91 minutes it judged on.
    const auto out = at(11, 1);
    REQUIRE(out.size() == 1);
    CHECK(out[0].ever);
    CHECK(out[0].age_ms == 1'081 * kMin);   // just over 18 h, honestly reported
}

TEST_CASE("armed: the cap costs at most one grace period, once, at the open") {
    // The whole price of the gate, stated exactly. The cap can only bite while
    // the market has been open for less than the grace period, so the only page
    // it can ever move is a session's FIRST one, and only when the last delivery
    // predates the open. Everything after that is untouched.
    //
    // 2026-08-07 replayed, minutes from 09:00: last good SOXS refresh 09:17, a
    // 90-minute grace, and the market open at 09:30 (t = 30).
    HistoryFreshness f;
    f.record("SOXS", "5m", 17 * kMin);
    const auto syms = every30({"SOXS"});
    auto armed_at = [](int64_t t) { return hist_armed_ms(t, t - 30 * kMin); };
    // Ungated it would have paged at 10:48, a grace period past the last delivery.
    CHECK(f.stale(syms, "5m", 108 * kMin, 108 * kMin).size() == 1);
    // Gated, that moment is a grace period of OPEN MARKET instead: 11:01.
    CHECK(f.stale(syms, "5m", 108 * kMin, armed_at(108 * kMin)).empty());
    CHECK(f.stale(syms, "5m", 121 * kMin, armed_at(121 * kMin)).size() == 1);
    // Thirteen minutes, on a stall that ran until 14:01 before anyone saw it.
    CHECK(f.stale(syms, "5m", 301 * kMin, armed_at(301 * kMin)).size() == 1);
    // A stall that starts after the open pays nothing at all: last delivery
    // 11:30 (t = 150), grace 90, so it pages at 13:00 (t = 240) either way.
    HistoryFreshness g;
    g.record("SOXS", "5m", 150 * kMin);
    CHECK(g.stale(syms, "5m", 239 * kMin, armed_at(239 * kMin)).empty());
    CHECK(g.stale(syms, "5m", 241 * kMin, armed_at(241 * kMin)).size() == 1);
    CHECK(g.stale(syms, "5m", 241 * kMin, 241 * kMin).size() == 1);   // ungated: same
}

TEST_CASE("armed: a half-day shuts the gate at 13:00") {
    // Three days a year the NYSE closes at 1pm. Without the early-close rule the
    // watchdog pages through the afternoon on each of them.
    HistoryFreshness f;
    f.record("MUU", "5m", 0);
    const auto syms = every30({"MUU"});
    auto armed = [&](int y, int m, int d, int hh, int mm) {
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
        tm.tm_wday = tt::weekday_of(y, m, d);
        tm.tm_hour = hh; tm.tm_min = mm;
        return hist_armed_ms(600 * kMin, tt::rth_open_elapsed_ms(tm));
    };
    const int64_t now = 200 * kMin;   // MUU is 200 minutes stale either way
    // 2026-11-27, the Friday after Thanksgiving: still open at 12:30...
    CHECK(f.stale(syms, "5m", now, armed(2026, 11, 27, 12, 30)).size() == 1);
    CHECK(f.stale(syms, "5m", now, armed(2026, 11, 27, 14, 30)).empty());
    // ...while the ordinary Friday a week later runs to 16:00.
    CHECK(f.stale(syms, "5m", now, armed(2026, 12, 4, 14, 30)).size() == 1);
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
