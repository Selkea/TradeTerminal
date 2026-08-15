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

// stale() takes the session's age and the ARMED window separately. Most cases
// below are mid-session on an open market, where the two coincide; the ones
// that pull them apart are the RTH-gate block further down.
static std::vector<StaleBars> stale_at(const HistoryFreshness& f,
                                       const std::vector<WatchedSymbol>& syms,
                                       int64_t now, int64_t window) {
    return f.stale(syms, "5m", now, now, window, window);
}

TEST_CASE("stale: nothing is reported while every symbol refreshes") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    f.record("AAOX", "5m", 0);
    const auto syms = every30({"SOXS", "AAOX"});
    // 89 minutes old on a 90-minute grace: still quiet.
    CHECK(stale_at(f, syms, 89 * kMin, 89 * kMin).empty());
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
    const auto out = stale_at(f, syms, 301 * kMin, 301 * kMin);
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
    const auto out = stale_at(f, syms, 301 * kMin, 301 * kMin);
    REQUIRE(out.size() == 1);
    CHECK(out[0].symbol == "SOXS");
    CHECK(out[0].age_ms == 284 * kMin);

    // ...and the slow one is judged by its own cadence, NOT SILENCED FOREVER —
    // which this case has always claimed and, between 0.21.0 and 0.21.1, did
    // not check. It asserted it with an 800-minute armed window, and
    // pump_history_watchdog cannot build one: the window is the time the market
    // has been open, which peaks at 389 minutes on a full day and 209 on a 1pm
    // half-day. 0.21.0 capped every symbol's age at that same window, so
    // SNDQ's 720-minute grace was unreachable and it could not be reported
    // stale at any second of any trading day — probed over all 23400 seconds of
    // RTH with it five days dead: zero. So drive it with the widest window the
    // real caller can ever produce, and with the narrowest kind of day too.
    const int64_t days_live = 4 * 1440 * kMin;
    const int64_t full_day = 389 * kMin;    // 09:30 -> 15:59
    const int64_t half_day = 209 * kMin;    // 09:30 -> 12:59, the three 1pm closes
    const auto both = f.stale(syms, "5m", 800 * kMin, 800 * kMin, days_live, full_day);
    REQUIRE(both.size() == 2);
    CHECK(both[0].symbol == "SOXS");
    CHECK(both[1].symbol == "SNDQ");
    CHECK(f.stale(syms, "5m", 800 * kMin, 800 * kMin, days_live, half_day).size() == 2);
}

TEST_CASE("stale: a symbol that has NEVER been answered is aged from the session") {
    // The failure can start before a symbol's first refresh lands, so "no
    // delivery ever" must not read as healthy. It also means the grace doubles
    // as a settle-in window: nothing pages in the first 90 minutes of a session.
    HistoryFreshness f;
    const auto syms = every30({"SOXS"});
    CHECK(stale_at(f, syms, 500 * kMin, 89 * kMin).empty());
    const auto out = stale_at(f, syms, 500 * kMin, 91 * kMin);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].ever);
    CHECK(out[0].age_ms == 91 * kMin);
    // The SESSION's age, not the armed window's: a session left running
    // overnight is entitled to page for a symbol it has never once been served,
    // however short today's open has been so far. 0.21.0 aged it from the armed
    // window instead, which is why a slow-cadence symbol could never get here.
    const auto never = f.stale(syms, "5m", 500 * kMin, 500 * kMin, 3 * 1440 * kMin, 46 * kMin);
    REQUIRE(never.size() == 1);
    CHECK(never[0].age_ms == 3 * 1440 * kMin);
}

TEST_CASE("stale: a symbol outside the watched set is never reported") {
    // Only a symbol on an autopilot TIMER has a refresh cadence. One that is
    // unarmed — or armed on the "Drawdown" trigger alone, which pump_autopilot
    // never runs a timed cycle for — is stale forever by design and would page
    // every session. pump_history_watchdog keeps both out of this list.
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    CHECK(f.stale({}, "5m", 500 * kMin, 500 * kMin, 500 * kMin, 500 * kMin).empty());
}

TEST_CASE("stale: a fresh delivery clears the whole condition") {
    HistoryFreshness f;
    f.record("SOXS", "5m", 0);
    const auto syms = every30({"SOXS"});
    CHECK(stale_at(f, syms, 200 * kMin, 200 * kMin).size() == 1);
    f.record("SOXS", "5m", 200 * kMin);
    CHECK(stale_at(f, syms, 200 * kMin, 200 * kMin).empty());
}

TEST_CASE("stale: a stopped session's deliveries do not follow it into the next") {
    // The terminal stays up across the 15:55 scheduled stop and the 09:25
    // auto-start. Without the reset, yesterday's 15:32 delivery is ~17 hours old
    // on the first frame of a healthy morning — and while the arming settle-in
    // now holds that off the phone for 45 minutes (below), clear() is what keeps
    // /diag's age and refreshing()'s evidence honest, so both are pinned.
    HistoryFreshness f;
    f.record("SOXS", "5m", 392 * kMin);   // 15:32, counting minutes from 09:00
    const auto syms = every30({"SOXS"});
    const int64_t next_open = 1'465 * kMin;   // 09:25 the next morning
    // A fresh session is one minute old, so nothing is judged yet either way.
    CHECK(stale_at(f, syms, next_open, 1 * kMin).empty());
    // ...but the carried-over delivery is still visible as an age, and would
    // still be offered as proof the session is serving history if it were recent.
    CHECK(f.age_ms("SOXS", "5m", next_open) == 1'073 * kMin);
    f.clear();
    CHECK(f.age_ms("SOXS", "5m", next_open) == -1);
    // Now it reads as never-answered and the grace acts as the settle-in window.
    CHECK(stale_at(f, syms, next_open, 1 * kMin).empty());
    CHECK(stale_at(f, syms, next_open, 91 * kMin).size() == 1);
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
    const int64_t session = 400 * kMin;
    // Armed (mid-session), this is exactly what the operator was paged about.
    CHECK(f.stale(syms, "5m", now, now, session, 400 * kMin).size() == 4);
    // Closed: the same evidence, judged against a shut market.
    CHECK(f.stale(syms, "5m", now, now, session, 0).empty());
    // ...and it stays empty as the night wears on, which is the whole point:
    // the 30-minute re-alert cannot find anything to re-page.
    CHECK(f.stale(syms, "5m", 900 * kMin, 900 * kMin, 900 * kMin, 0).empty());
}

TEST_CASE("armed: the open boundary DEFERS an overnight-stale symbol, then says it") {
    // The case the gate is most likely to get wrong, in both directions. A
    // session that was never stopped is still holding yesterday's deliveries, so
    // at 09:30:01 a symbol last served at 17:00 is sixteen hours old and a bare
    // is-it-open boolean would page on the first frame the gate opened.
    //
    // But it must not be forgiven forever either, and 0.21.0 came close to
    // doing that: pump_autopilot disarms only on !live_running, so a session
    // left running was still cycling and still asking for MUU's bars all night.
    // A delivery that predates the open therefore means those fetches FAILED —
    // the 2026-08-07 stall, in the one session shape most likely to hide it.
    // The settle-in is a fixed 45 minutes of open market, not a per-symbol
    // grace: enough for the morning to land, not enough to swallow the failure.
    HistoryFreshness f;
    f.record("MUU", "5m", 480 * kMin);   // 17:00 yesterday
    const auto syms = every30({"MUU"});   // production cadence: a 90-minute grace
    // Minutes from yesterday 09:00, so TODAY 09:30 is t = 1470.
    auto today = [](int hh, int mm) {
        return static_cast<int64_t>(1440 + hh * 60 + mm - 540) * kMin;
    };
    // Live since yesterday 09:25 (t = 25) and never stopped.
    auto session_at = [&](int hh, int mm) { return today(hh, mm) - 25 * kMin; };
    auto armed_at = [&](int hh, int mm) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 7; tm.tm_mday = 12;   // Wednesday
        tm.tm_wday = tt::weekday_of(2026, 8, 12);
        tm.tm_hour = hh; tm.tm_min = mm;
        return hist_armed_ms(session_at(hh, mm), tt::rth_open_elapsed_ms(tm));
    };
    auto at = [&](int hh, int mm) {
        return f.stale(syms, "5m", today(hh, mm), today(hh, mm), session_at(hh, mm), armed_at(hh, mm));
    };
    CHECK(at(9, 30).empty());    // the instant the gate opens: 16.5 h "stale"
    CHECK(at(10, 14).empty());   // 44 minutes of open market: still settling in
    // 46 minutes of open market with nothing delivered IS the failure, so it
    // pages then — and reports the TRUE age, not the window it judged on.
    const auto out = at(10, 16);
    REQUIRE(out.size() == 1);
    CHECK(out[0].ever);
    CHECK(out[0].age_ms == 1'036 * kMin);   // just over 17 h, honestly reported
    // It keeps saying so all day, and stops the moment the market shuts.
    CHECK(at(15, 59).size() == 1);
    CHECK(at(16, 0).empty());
}

TEST_CASE("armed: the gate costs a fixed 45 minutes, once, at the open") {
    // The whole price of the gate, stated exactly, because it had to be paid
    // for before going near the 2026-08-07 detection. It is a FIXED window, so
    // it does not scale with a symbol's grace and cannot make one unpageable —
    // that was the 0.21.0 defect (see kHistArmSettleMs).
    //
    // 2026-08-07 replayed, minutes from 09:00: last good SOXS refresh 09:17, a
    // 90-minute grace, market open at 09:30 (t = 30), session live from 09:25.
    HistoryFreshness f;
    f.record("SOXS", "5m", 17 * kMin);
    const auto syms = every30({"SOXS"});
    auto win = [&](int64_t t) {
        const int64_t open = t > 30 * kMin ? t - 30 * kMin : 0;
        return hist_armed_ms(t - 25 * kMin, open);
    };
    // It paged at 10:48 ungated, and it still pages at 10:48: by then the
    // market had been open 78 minutes, so the settle-in was long since done.
    CHECK(stale_at(f, syms, 107 * kMin, 107 * kMin).empty());
    CHECK(stale_at(f, syms, 108 * kMin, 108 * kMin).size() == 1);
    CHECK(f.stale(syms, "5m", 107 * kMin, 107 * kMin, 82 * kMin, win(107 * kMin)).empty());
    CHECK(f.stale(syms, "5m", 108 * kMin, 108 * kMin, 83 * kMin, win(108 * kMin)).size() == 1);
    // A stall that starts after the open pays nothing at all: last delivery
    // 11:30 (t = 150), grace 90, so it pages at 13:00 (t = 240) either way.
    HistoryFreshness g;
    g.record("SOXS", "5m", 150 * kMin);
    CHECK(g.stale(syms, "5m", 239 * kMin, 239 * kMin, 214 * kMin, win(239 * kMin)).empty());
    CHECK(g.stale(syms, "5m", 241 * kMin, 241 * kMin, 216 * kMin, win(241 * kMin)).size() == 1);
    CHECK(stale_at(g, syms, 241 * kMin, 241 * kMin).size() == 1);   // ungated: same

    // Where it DOES bite: a session live since 07:00 that has never been served
    // anything. Ungated it paged at 08:30, ninety minutes in, about a market
    // that was shut for every one of them. The cost is 45 minutes after the
    // open and nothing else — notably NOT a function of when the session
    // started, which is a free-form config string (trade_sched_start).
    HistoryFreshness h;
    auto at = [&](int hh, int mm) {
        const int64_t t = hh * 60 + mm;                        // local minutes
        const int64_t session = (t - 7 * 60) * kMin;           // live since 07:00
        const int64_t open = t > 570 ? (t - 570) * kMin : 0;   // 09:30
        return h.stale(syms, "5m", 0, 0, session, hist_armed_ms(session, open));
    };
    CHECK(stale_at(h, syms, 0, 91 * kMin).size() == 1);   // ungated: 08:31
    CHECK(at(8, 31).empty());
    CHECK(at(10, 14).empty());
    REQUIRE(at(10, 16).size() == 1);
    CHECK_FALSE(at(10, 16)[0].ever);
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
    const int64_t session = 600 * kMin;
    // 2026-11-27, the Friday after Thanksgiving: still open at 12:30...
    CHECK(f.stale(syms, "5m", now, now, session, armed(2026, 11, 27, 12, 30)).size() == 1);
    CHECK(f.stale(syms, "5m", now, now, session, armed(2026, 11, 27, 14, 30)).empty());
    // ...while the ordinary Friday a week later runs to 16:00.
    CHECK(f.stale(syms, "5m", now, now, session, armed(2026, 12, 4, 14, 30)).size() == 1);
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
    const auto stale = stale_at(f, syms, now, now);
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
    const int64_t now = 47 * kMin;   // two minutes past the arming settle-in
    const auto stale = stale_at(f, syms, now, now);
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

// ---------------------------------------------------------------------------
// A SERIES THAT STOPS ADVANCING WHILE ITS DELIVERIES ARRIVE ON TIME.
//
// 2026-08-14. STKH's 5-minute series was pinned at exactly x10010 bars from
// 10:44:49 through 15:44:49 — five hours — while MUU's grew the expected six
// bars per half hour (x9681 -> x9747). After the close STKH's count began to
// SHRINK (x10007, x10001, x9997) as the 6-month window slid off the back with
// nothing being appended at the front. Every fetch succeeded, on schedule, and
// returned the same newest bar; the watchdog reported "still refreshing MUU,
// SNDQ, STKH, SNDU" throughout, and /diag published the DELIVERY age under the
// field name last_bar_age_ms, which is a claim about a bar.
//
// This class measured freshness as "time since a batch ARRIVED", so a source
// that keeps answering with frozen data reads perfectly fresh. The 2026-08-07
// outage the file was written for is the opposite failure — requests that stop
// being answered — and the delivery age catches that one. Both are real, so both
// are measured, and the watchdog judges on the one that is about the DATA.

TEST_CASE("hist: a frozen series is stale even though every fetch succeeds") {
    HistoryFreshness f;
    const std::vector<WatchedSymbol> syms{{"STKH", 30}, {"MUU", 30}};
    // 45 minutes of open market, so the arming settle-in is cleared and the
    // watchdog is entitled to judge.
    const int64_t armed = 200 * kMin;
    const int64_t session = 200 * kMin;

    // Both symbols delivered punctually every 5 minutes for two hours. MUU's
    // newest bar advances with the clock; STKH's does not move at all — the
    // exact shape of x10010 sitting still while the fetches keep succeeding.
    const int64_t wall0 = 1'700'000'000'000LL;
    const int64_t stkh_frozen_bar = wall0;
    for (int i = 0; i <= 24; ++i) {
        const int64_t delivered = i * 5 * kMin;
        f.record("STKH", "5m", delivered, stkh_frozen_bar);
        f.record("MUU", "5m", delivered, wall0 + delivered);
    }
    const int64_t now_steady = 24 * 5 * kMin;      // last delivery was just now
    const int64_t now_wall = wall0 + now_steady;

    // The delivery age says both are perfect. That is the measurement that ran
    // for five hours and said nothing.
    CHECK(f.age_ms("STKH", "5m", now_steady) == 0);
    CHECK(f.age_ms("MUU", "5m", now_steady) == 0);
    CHECK(f.refreshing(syms, "5m", now_steady).size() == 2);

    // The BAR age separates them.
    CHECK(f.bar_age_ms("MUU", "5m", now_wall) == 0);
    CHECK(f.bar_age_ms("STKH", "5m", now_wall) == 120 * kMin);

    const auto stale = f.stale(syms, "5m", now_steady, now_wall, session, armed);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].symbol == "STKH");
    // It reports the BAR's age, not the delivery's, and says which it is —
    // "300m" and "300m frozen" are different faults with different fixes, and
    // the operator cannot tell them apart from a bare number.
    CHECK(stale[0].age_ms == 120 * kMin);
    CHECK(stale[0].frozen);
    CHECK(stale[0].ever);
}

TEST_CASE("hist: a healthy series is never called frozen") {
    HistoryFreshness f;
    const std::vector<WatchedSymbol> syms{{"MUU", 30}};
    const int64_t wall0 = 1'700'000'000'000LL;
    for (int i = 0; i <= 24; ++i)
        f.record("MUU", "5m", i * 5 * kMin, wall0 + i * 5 * kMin);
    const int64_t now_steady = 24 * 5 * kMin;
    CHECK(f.stale(syms, "5m", now_steady, wall0 + now_steady, 200 * kMin, 200 * kMin)
              .empty());
    // One missed 5-minute bar is not a stall. The threshold is three bar
    // intervals or the symbol's cadence grace, whichever is LARGER, so a normal
    // gap cannot page.
    CHECK(f.stale(syms, "5m", now_steady, wall0 + now_steady + 10 * kMin, 200 * kMin,
                  200 * kMin)
              .empty());
}

TEST_CASE("hist: the bar-age threshold scales with the interval") {
    // On a "1d" series the newest bar is legitimately hours or a whole weekend
    // old. Judging it against the 45-minute floor that is right for 5-minute
    // bars would page every single morning — the "wrong every day" failure this
    // file already documents for the gateway-restart advice.
    HistoryFreshness f;
    const std::vector<WatchedSymbol> syms{{"MUU", 30}};
    const int64_t wall0 = 1'700'000'000'000LL;
    const int64_t day = 24 * 60 * kMin;
    f.record("MUU", "1d", 0, wall0);
    // A day-old daily bar is normal.
    CHECK(f.stale(syms, "1d", 0, wall0 + day, 200 * kMin, 200 * kMin).empty());
    // Four days is not.
    const auto stale = f.stale(syms, "1d", 0, wall0 + 4 * day, 200 * kMin, 200 * kMin);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].frozen);
    CHECK(hist_interval_ms("5m") == 5 * kMin);
    CHECK(hist_interval_ms("1h") == 60 * kMin);
    CHECK(hist_interval_ms("1d") == day);
    // An interval nobody recognises is not judged on data age at all. Inventing
    // a bound would be worse than the blind spot.
    CHECK(hist_interval_ms("15m") == 0);
    f.record("MUU", "15m", 0, wall0);
    const std::vector<WatchedSymbol> odd{{"MUU", 30}};
    CHECK(f.stale(odd, "15m", 0, wall0 + 100 * day, 200 * kMin, 200 * kMin).empty());
}

TEST_CASE("hist: a caller with no bar timestamp cannot certify the data as fresh") {
    // record()'s newest_bar_ms defaults to 0 for an empty batch and for any call
    // site that has not been taught the question. That must leave the bar age
    // UNKNOWN rather than reset it — a delivery carrying no bar is not evidence
    // that the series advanced.
    HistoryFreshness f;
    const int64_t wall0 = 1'700'000'000'000LL;
    f.record("STKH", "5m", 0);
    CHECK(f.bar_age_ms("STKH", "5m", wall0) == -1);   // never answered
    CHECK(f.age_ms("STKH", "5m", 0) == 0);            // ...but the delivery landed
    // ...and once a real bar has arrived, a later bar-less delivery does not
    // silently overwrite it with "now".
    f.record("STKH", "5m", 0, wall0);
    f.record("STKH", "5m", 60 * kMin);
    CHECK(f.bar_age_ms("STKH", "5m", wall0 + 60 * kMin) == 60 * kMin);
    // Never negative: a source that hands over the in-progress bar of the
    // current interval legitimately stamps it ahead of now, and a negative age
    // slips under every `>` threshold a rule can write.
    CHECK(f.bar_age_ms("STKH", "5m", wall0 - 5 * kMin) == 0);
    // Freshness belongs to a SESSION, so clear() must drop both maps or a dead
    // session's bar would stand as evidence about the next one.
    f.clear();
    CHECK(f.bar_age_ms("STKH", "5m", wall0) == -1);
}

TEST_CASE("hist: the page names a frozen series as such") {
    // The old wording sends the operator to look for failing requests ("check
    // the tws-data 'retrying as req' lines"). When the fetches are SUCCEEDING on
    // time and the series is not advancing there are no such lines to find, and
    // a reader who goes looking concludes the page was spurious.
    const std::vector<StaleBars> frozen{{"STKH", 300 * kMin, true, true}};
    const std::string msg =
        hist_stall_alert("STKH 300m frozen", frozen, {"MUU", "STKH"}, true);
    CHECK(msg.find("STOPPED ADVANCING") != std::string::npos);
    CHECK(msg.find("frozen SERIES") != std::string::npos);
    CHECK(msg.find("do NOT restart the gateway") != std::string::npos);
    // The ordinary "nothing is arriving" stall keeps its own, different text.
    const std::vector<StaleBars> dead{{"SOXS", 300 * kMin, true, false}};
    const std::string old = hist_stall_alert("SOXS 300m", dead, {"MUU"}, true);
    CHECK(old.find("STOPPED ADVANCING") == std::string::npos);
    CHECK(old.find("have stopped refreshing") != std::string::npos);
}
