// US market calendar. Only one thing depends on it — whether the 08:45 pre-open
// gateway check runs — but that check kills and relaunches IB Gateway, which
// spends a login attempt on an account IBKR locks after repeated failures. A
// wrong "yes" on Thanksgiving spends one for nothing, into IBKR's holiday
// maintenance window; a wrong "no" on a real trading day puts back the 13-hour
// blindness of 2026-08-09. So both directions are pinned here.
#include "doctest.h"

#include "market_calendar.h"

#include <fstream>
#include <iterator>
#include <string>

using namespace tt;

// A std::tm the way localtime_s fills one in, so the tests drive the exact
// entry point App::pump_preopen_gateway_check calls.
static std::tm mk(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_wday = weekday_of(y, m, d);
    // localtime_s fills this in too, and the session guard's once-per-day stamp
    // is keyed on it — a helper that left it 0 would make every date in a year
    // compare equal and quietly pass a test that should fail.
    tm.tm_yday =
        static_cast<int>(days_from_civil(y, m, d) - days_from_civil(y, 1, 1));
    return tm;
}

// ...and the same with a time of day, for the session-hours half.
static std::tm at(int y, int m, int d, int hh, int mm) {
    std::tm tm = mk(y, m, d);
    tm.tm_hour = hh;
    tm.tm_min = mm;
    return tm;
}

static constexpr int64_t kMin = 60'000;
static constexpr int64_t kHour = 60 * kMin;

TEST_CASE("calendar: weekday arithmetic") {
    CHECK(weekday_of(1970, 1, 1) == 4);    // Thursday
    CHECK(weekday_of(2000, 2, 29) == 2);   // Tuesday, leap day
    CHECK(weekday_of(2026, 8, 9) == 0);    // the incident: a Sunday
    CHECK(weekday_of(2026, 1, 1) == 4);
    CHECK(nth_weekday(2026, 1, 1, 3) == 19);    // MLK 2026
    CHECK(nth_weekday(2026, 11, 4, 4) == 26);   // Thanksgiving 2026
    CHECK(last_weekday(2026, 5, 1, 31) == 25);  // Memorial Day 2026
}

TEST_CASE("calendar: the 2026 NYSE holiday list, exactly") {
    const int hol[][2] = {{1, 1},  {1, 19}, {2, 16}, {4, 3},  {5, 25},
                          {6, 19}, {7, 3},  {9, 7},  {11, 26}, {12, 25}};
    for (const auto& h : hol) {
        INFO("2026-", h[0], "-", h[1]);
        CHECK(is_us_market_holiday(2026, h[0], h[1]));
        CHECK_FALSE(is_us_trading_day(mk(2026, h[0], h[1])));
    }
    // July 4 2026 is a Saturday, so the market closes the Friday BEFORE it —
    // and July 4 itself is not separately a holiday, it is just a weekend.
    CHECK_FALSE(is_us_market_holiday(2026, 7, 4));
    // The day after Thanksgiving and Christmas Eve are EARLY closes, not
    // closures: the market opens at 09:30, so the pre-open check must run.
    CHECK(is_us_trading_day(mk(2026, 11, 27)));
    CHECK(is_us_trading_day(mk(2026, 12, 24)));
}

TEST_CASE("calendar: Good Friday moves with Easter") {
    // Good Friday is the only moveable full closure, and the only one whose
    // date cannot be derived from a weekday-of-month rule.
    CHECK(is_us_market_holiday(2024, 3, 29));
    CHECK(is_us_market_holiday(2025, 4, 18));
    CHECK(is_us_market_holiday(2026, 4, 3));
    CHECK(is_us_market_holiday(2027, 3, 26));
    CHECK(is_us_market_holiday(2038, 4, 23));   // latest it can fall
    // Easter on April 1 or 2 puts Good Friday back into March: 2018 (Apr 1).
    CHECK(is_us_market_holiday(2018, 3, 30));
}

TEST_CASE("calendar: the Saturday/Sunday observance rules") {
    // Sunday -> Monday.
    CHECK(is_us_market_holiday(2022, 12, 26));   // Christmas fell on a Sunday
    CHECK(is_us_market_holiday(2023, 1, 2));     // New Year's fell on a Sunday
    CHECK(is_us_market_holiday(2021, 7, 5));     // July 4 fell on a Sunday
    // Saturday -> the preceding Friday...
    CHECK(is_us_market_holiday(2020, 7, 3));     // July 4 fell on a Saturday
    CHECK(is_us_market_holiday(2021, 12, 24));   // Christmas fell on a Saturday
    // ...EXCEPT New Year's Day, which is simply not observed then. Jan 1 2022
    // was a Saturday and Dec 31 2021 was a full trading day.
    CHECK_FALSE(is_us_market_holiday(2021, 12, 31));
    CHECK(is_us_trading_day(mk(2021, 12, 31)));
}

TEST_CASE("calendar: ordinary trading days are not holidays") {
    // The direction that matters most: over-flagging silently reinstates the
    // blindness this whole feature exists to remove.
    int weekdays = 0, trading = 0;
    for (int m = 1; m <= 12; ++m) {
        for (int d = 1; d <= 28; ++d) {
            const std::tm tm = mk(2026, m, d);
            if (tm.tm_wday >= 1 && tm.tm_wday <= 5) ++weekdays;
            if (is_us_trading_day(tm)) ++trading;
        }
    }
    // The first 28 of every month is exactly four weeks, so 20 weekdays each.
    CHECK(weekdays == 240);
    // All ten 2026 closures land in a day-of-month <= 28 and all ten are
    // weekdays, so the calendar may remove exactly ten days and no more. One
    // over-flagged day is one morning the gateway goes unchecked.
    CHECK(trading == 230);
    CHECK(is_us_trading_day(mk(2026, 8, 10)));   // the Monday after the incident
    CHECK_FALSE(is_us_trading_day(mk(2026, 8, 9)));
}

// ---- session hours ---------------------------------------------------------
// Added 0.21.0 for the history-staleness watchdog, which paged Critical at
// 18:40 and 19:10 on 2026-08-11 — "strategies are trading on stale candles" —
// with the market shut since 16:00 and the book flat. Both directions matter as
// much as they do above: a wrong "closed" silences the watchdog during the
// hours the 2026-08-07 outage happened in.

TEST_CASE("hours: an ordinary trading day opens at 09:30 and shuts at 16:00") {
    // 2026-08-11, the incident day: a Tuesday, not a holiday, not a half day.
    CHECK(us_market_close_h(mk(2026, 8, 11)) == 16.0);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 9, 29)) == 0);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 9, 30)) == 0);   // open, nothing late yet
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 10, 15)) == 45 * kMin);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 12, 0)) == 2 * kHour + 30 * kMin);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 15, 59)) == 6 * kHour + 29 * kMin);
    // The close is the gate closing, to the minute.
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 16, 0)) == 0);
}

TEST_CASE("hours: the two pages of 2026-08-11 land outside the session") {
    // The regression, stated as the two alert timestamps from the operator's
    // phone. A live session was left running past the 15:55 auto-stop; these
    // were 30 minutes apart and would have repeated until morning.
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 18, 40)) == 0);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 11, 19, 10)) == 0);
    // ...and the small hours of a session that never stopped.
    CHECK(rth_open_elapsed_ms(at(2026, 8, 12, 3, 0)) == 0);
    // But the morning after re-arms: the same session, 45 minutes past the open.
    CHECK(rth_open_elapsed_ms(at(2026, 8, 12, 10, 15)) == 45 * kMin);
}

TEST_CASE("hours: nothing is open on a weekend or a holiday") {
    CHECK(us_market_close_h(mk(2026, 8, 8)) == 0.0);           // Saturday
    CHECK(rth_open_elapsed_ms(at(2026, 8, 8, 11, 0)) == 0);
    CHECK(rth_open_elapsed_ms(at(2026, 8, 9, 11, 0)) == 0);    // Sunday
    CHECK(rth_open_elapsed_ms(at(2026, 9, 7, 11, 0)) == 0);    // Labor Day
    CHECK(rth_open_elapsed_ms(at(2026, 11, 26, 11, 0)) == 0);  // Thanksgiving
    CHECK(rth_open_elapsed_ms(at(2026, 4, 3, 11, 0)) == 0);    // Good Friday
}

TEST_CASE("hours: the three NYSE half-days shut at 13:00") {
    // Without these the watchdog pages for three hours on each of them — the
    // same false page, three more days a year.
    CHECK(is_us_early_close(2026, 11, 27));   // the Friday after Thanksgiving
    CHECK(is_us_early_close(2026, 12, 24));   // Christmas Eve, a Thursday
    CHECK(is_us_early_close(2025, 7, 3));     // July 3, a Thursday
    CHECK(is_us_early_close(2024, 7, 3));     // ...a Wednesday
    CHECK(is_us_early_close(2023, 7, 3));     // ...a Monday
    CHECK(is_us_early_close(2019, 12, 24));
    CHECK(us_market_close_h(mk(2026, 11, 27)) == 13.0);
    CHECK(rth_open_elapsed_ms(at(2026, 11, 27, 12, 59)) == 3 * kHour + 29 * kMin);
    CHECK(rth_open_elapsed_ms(at(2026, 11, 27, 13, 0)) == 0);
    CHECK(rth_open_elapsed_ms(at(2026, 11, 27, 15, 30)) == 0);
    CHECK(rth_open_elapsed_ms(at(2026, 12, 24, 14, 0)) == 0);
}

TEST_CASE("hours: a half-day that the adjacent holiday swallowed is not one") {
    // When July 4 falls on a Saturday the NYSE shuts ALL DAY on Friday July 3,
    // and likewise Christmas Eve when Christmas falls on a Saturday. Calling
    // those "early closes" would be harmless here but wrong, and is_us_early_
    // close is the kind of predicate that gets reused.
    CHECK(is_us_market_holiday(2026, 7, 3));         // July 4 2026 is a Saturday
    CHECK_FALSE(is_us_early_close(2026, 7, 3));
    CHECK(is_us_market_holiday(2021, 12, 24));       // Christmas 2021 was a Saturday
    CHECK_FALSE(is_us_early_close(2021, 12, 24));
    CHECK(rth_open_elapsed_ms(at(2026, 7, 3, 11, 0)) == 0);
}

TEST_CASE("hours: a full day next to a holiday keeps its full session") {
    // The direction that costs coverage. When the July 4 holiday lands on a
    // MONDAY the preceding Friday is a normal 16:00 session, and so is the
    // Friday before a Monday-observed Christmas — the NYSE grants no early
    // close there, and inventing one would blind the watchdog for three hours.
    CHECK_FALSE(is_us_early_close(2021, 7, 2));   // July 4 2021 fell on a Sunday
    CHECK(us_market_close_h(mk(2021, 7, 2)) == 16.0);
    CHECK_FALSE(is_us_early_close(2022, 12, 23));   // Christmas 2022 fell on a Sunday
    CHECK(us_market_close_h(mk(2022, 12, 23)) == 16.0);
    CHECK_FALSE(is_us_early_close(2016, 12, 23));
    // A weekend "half day" is just a weekend.
    CHECK_FALSE(is_us_early_close(2021, 7, 3));   // a Saturday
    CHECK_FALSE(is_us_early_close(2022, 12, 24));   // a Saturday
}

TEST_CASE("hours: an ordinary day is never an early close") {
    // Over-flagging is the expensive mistake: three silent hours on a day the
    // 2026-08-07 stall could be running. Exactly three half-days in 2026.
    int early = 0;
    for (int m = 1; m <= 12; ++m)
        for (int d = 1; d <= 31; ++d)
            if (is_us_early_close(2026, m, d)) ++early;
    CHECK(early == 2);   // 2026 loses July 3 to the Saturday-July-4 closure
    int early25 = 0;
    for (int m = 1; m <= 12; ++m)
        for (int d = 1; d <= 31; ++d)
            if (is_us_early_close(2025, m, d)) ++early25;
    CHECK(early25 == 3);   // Jul 3, Nov 28, Dec 24
    CHECK(is_us_early_close(2025, 11, 28));
    CHECK(is_us_early_close(2025, 12, 24));
}

// ---- the entry gate ---------------------------------------------------------
// The 2026-08-13 incident in one predicate. Two strategies signalled entries at
// 16:00:12 and 16:00:17 ET and IBKR refused both ("Exchange is closed"); a third
// at 16:05:16 was ACCEPTED and rested overnight as a market order for the next
// open. Earlier the same week the same strategy entered KORU at 04:05 and IBKR
// filled it at 09:30, 5.4 hours after the signal that justified it. Every one of
// those instants must read false here.

TEST_CASE("entry gate: the three 2026-08-13 orders are all outside the window") {
    // 2026-08-13 was a Thursday, a normal full session.
    CHECK(is_us_trading_day(mk(2026, 8, 13)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 16, 0)));    // the two rejects
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 16, 5)));    // the resting order
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 4, 5)));     // the 5.4h-stale fill
    // ...while the fills that session made legitimately are inside it.
    CHECK(rth_entry_allowed(at(2026, 8, 13, 15, 25)));
    CHECK(rth_entry_allowed(at(2026, 8, 13, 15, 35)));
}

TEST_CASE("entry gate: the window is 09:30 up to the 15:57 EOD backstop") {
    // Closes EARLY, with the backstop, not with the exchange: the backstop is
    // edge-triggered and marks the day done when it fires, so a position opened
    // at 15:58 has nothing left that will close it today.
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 9, 29)));
    CHECK(rth_entry_allowed(at(2026, 8, 13, 9, 30)));
    CHECK(rth_entry_allowed(at(2026, 8, 13, 15, 56)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 15, 57)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 13, 15, 58)));
    CHECK(entry_cutoff_h(mk(2026, 8, 13)) == doctest::Approx(kEodBackstopH));
}

TEST_CASE("entry gate: nothing may be entered on a weekend or a holiday") {
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 15, 12, 0)));   // Saturday
    CHECK_FALSE(rth_entry_allowed(at(2026, 8, 16, 12, 0)));   // Sunday
    CHECK_FALSE(rth_entry_allowed(at(2026, 11, 26, 12, 0)));  // Thanksgiving
    CHECK_FALSE(is_us_trading_day(mk(2026, 7, 3)));           // holiday-swallowed
    CHECK_FALSE(rth_entry_allowed(at(2026, 7, 3, 12, 0)));
    CHECK(entry_cutoff_h(mk(2026, 8, 16)) == 0.0);
}

TEST_CASE("entry gate: a half-day cuts entries three minutes before its 13:00 close") {
    // The whole point of subtracting the lead from the DAY's close rather than
    // hardcoding 15:57: on an early close, 15:57 is three hours after the shop
    // shut. 2026-11-27 is the day after Thanksgiving.
    REQUIRE(is_us_early_close(2026, 11, 27));
    CHECK(entry_cutoff_h(mk(2026, 11, 27)) == doctest::Approx(kRthEarlyCloseH - 0.05));
    CHECK(rth_entry_allowed(at(2026, 11, 27, 12, 56)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 11, 27, 12, 57)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 11, 27, 13, 30)));
    CHECK_FALSE(rth_entry_allowed(at(2026, 11, 27, 15, 30)));
}

// ---- the session guard ------------------------------------------------------
//
// 2026-08-14: /diag at 17:45:50 ET — 1 h 46 m past the close — read
// live_running true, halted false. The 09:41 session had been started by the
// daily auto-lineup, and NOTHING on any clock could end it: pump_schedule
// returns on its first line when trade_sched_on is false (it was), the lineup
// auto-start never consults that flag, and the engine's 15:57 EOD backstop
// flattens without clearing live_running_. A human clicking "Update & restart"
// at 17:46:47 is what stopped it.
//
// Until this existed, NOTHING anywhere asserted that a live session ever ends.
// That absence is the finding; these cases are the assertion.

static std::tm at_s(int y, int m, int d, int hh, int mm, int ss) {
    std::tm tm = at(y, m, d, hh, mm);
    tm.tm_sec = ss;
    return tm;
}

TEST_CASE("session guard: a normal day ends the session at 16:15") {
    // 15 minutes after the close, not at it: the 15:57 backstop, a strategy's
    // own EOD flatten and the day's last fills all land around 16:00, and
    // stopping on top of them reaps the broker adapter mid-flight - the
    // 2026-08-13 lost-execution-report failure class.
    CHECK(session_end_h(mk(2026, 8, 14)) == doctest::Approx(16.25));
    const int cutoff = 16 * 3600 + 15 * 60;
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 16, 14, 59), -1, cutoff - 30));
    CHECK(session_should_stop(at_s(2026, 8, 14, 16, 15, 0), -1, cutoff - 1));
    // ...and it does NOT fire during the session it is guarding.
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 9, 41, 37), -1, 9 * 3600));
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 15, 57, 0), -1, 15 * 3600));
}

TEST_CASE("session guard: a session started AFTER the close is left alone") {
    // THE REGRESSION THIS PINS. The first version of this guard was LEVEL
    // triggered, so it fired on the first tick of any session begun after the
    // cutoff - which is every VPS deploy, because the evening is the only
    // window where deploying is safe (flat book, market closed). Paired with a
    // position-liquidating stop, an 18:30 deploy would have cancelled the
    // resting protective orders and fired a market flatten into a shut
    // exchange, seconds after coming up.
    //
    // Edge-triggered, the guard may only stop a session it WATCHED cross the
    // cutoff. prev_sod < 0 is the first observation of a fresh process.
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 18, 30, 0), -1, -1));
    const int deployed_at = 18 * 3600 + 30 * 60;
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 18, 30, 30), -1, deployed_at));
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 23, 59, 0), -1, deployed_at));
    // The operator's real 2026-08-14 restart at 17:47:38, for the same reason.
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 14, 17, 48, 0), -1,
                                    17 * 3600 + 47 * 60 + 38));
}

TEST_CASE("session guard: an early close is 13:15, not 16:15") {
    // A hardcoded 16:15 would be three hours late on the three NYSE half-days -
    // the same arithmetic entry_cutoff_h exists to get right. 2026-11-27 is the
    // day after Thanksgiving; 2026-12-24 is Christmas Eve.
    REQUIRE(is_us_early_close(2026, 11, 27));
    CHECK(session_end_h(mk(2026, 11, 27)) == doctest::Approx(13.25));
    const int early = 13 * 3600 + 15 * 60;
    CHECK_FALSE(session_should_stop(at_s(2026, 11, 27, 13, 14, 59), -1, early - 30));
    CHECK(session_should_stop(at_s(2026, 11, 27, 13, 15, 0), -1, early - 1));
    REQUIRE(is_us_early_close(2026, 12, 24));
    CHECK(session_end_h(mk(2026, 12, 24)) == doctest::Approx(13.25));
    CHECK(session_should_stop(at_s(2026, 12, 24, 14, 0, 0), -1, early - 1));
}

TEST_CASE("session guard: a non-trading day is not a cutoff of its own") {
    // The first version returned true from the first tick of any weekend or
    // holiday, which is the same level-trigger defect wearing a second face: it
    // fights a session a human deliberately started on a Saturday, and such a
    // session cannot trade anyway (the entry gate refuses every entry outside
    // RTH). An UNATTENDED session running into Saturday was already stopped by
    // Friday's crossing, so the arm caught nothing the edge does not.
    CHECK(session_end_h(mk(2026, 8, 15)) == 0.0);                  // Saturday
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 15, 3, 0, 0), -1, 2 * 3600));
    CHECK_FALSE(session_should_stop(at_s(2026, 8, 16, 12, 0, 0), -1, 11 * 3600));
    CHECK_FALSE(session_should_stop(at_s(2026, 11, 26, 10, 0, 0), -1, 9 * 3600));
    // Friday's crossing is what actually catches the unattended case.
    CHECK(session_should_stop(at_s(2026, 8, 14, 16, 15, 0), -1,
                              16 * 3600 + 14 * 60 + 59));
}

TEST_CASE("session guard: it fires once a day, so a deliberate restart survives") {
    // The guard is a backstop against a session nobody is watching, not a lock
    // on the Start button. On 2026-08-14 a human DID start a fresh session at
    // 17:47:38, after the close, on purpose; fighting that every tick would make
    // the app unusable to the person who owns it.
    const int cutoff = 16 * 3600 + 15 * 60;
    const std::tm crossing = at_s(2026, 8, 14, 16, 15, 0);
    CHECK(session_should_stop(crossing, -1, cutoff - 1));
    CHECK_FALSE(session_should_stop(crossing, crossing.tm_yday, cutoff - 1));
    // ...and the stamp does not leak into the next day.
    const std::tm tomorrow = at_s(2026, 8, 17, 16, 15, 0);
    CHECK(session_should_stop(tomorrow, crossing.tm_yday, cutoff - 1));
}

TEST_CASE("session guard: it does not depend on the panel schedule at all") {
    // The entire defect was that the only clock-driven terminator in the app
    // could be switched off, while the thing that STARTS unattended sessions
    // could not. This is a pure function of the clock and the calendar: there is
    // no configuration input to switch off. Stated as a test because "it has no
    // arguments to disable it" is exactly the property that was missing.
    // The VPS's actual 2026-08-14 setting was trade_sched_stop "20:00" - even an
    // ARMED schedule would not have stopped that session until then, whereas the
    // guard ends it at the crossing regardless.
    CHECK(session_should_stop(at_s(2026, 8, 14, 16, 15, 0), -1,
                              16 * 3600 + 15 * 60 - 1));
}

// ---------------------------------------------------------------------------
// THE BACKSTOP CLOSES WHEN THE GATE CLOSES — ON EVERY KIND OF DAY.
//
// A parity audit found the two ends of the session judged by different clocks:
// entry_cutoff_h walks the calendar and stops entries at 12:57 on an early
// close, while the engine's EOD backstop compared the wall clock to a hardcoded
// 15.95. On the three 13:00 days those are 2 h 57 m apart, and both sides of
// that gap are wrong — either the position carries overnight under a paused
// strategy, or the backstop fires at 15:57 and sends cancel_all plus an RTH
// market flatten into an exchange shut since 13:00.
//
// LiveConfig::eod_flatten_h_for_day now takes its hour from this same function,
// which is not a coincidence to be re-derived later: entry_cutoff_h is DEFINED
// as the last minute a position may be opened because it is the last minute
// something will still close it. The hour that closes it is therefore the same
// hour. These pin that identity on all three day shapes.

TEST_CASE("eod backstop: a normal session still flattens at 15:57") {
    CHECK(entry_cutoff_h(mk(2026, 8, 13)) == doctest::Approx(kEodBackstopH));
    CHECK(entry_cutoff_h(mk(2026, 8, 17)) == doctest::Approx(kEodBackstopH));
}

TEST_CASE("eod backstop: an early close flattens before the exchange shuts") {
    // 12:57, three minutes BEFORE the 13:00 close — not 15:57, which is nearly
    // three hours after it. The whole point: the flatten must be able to fill.
    REQUIRE(is_us_early_close(2026, 11, 27));
    REQUIRE(is_us_early_close(2026, 12, 24));
    CHECK(entry_cutoff_h(mk(2026, 11, 27)) == doctest::Approx(kRthEarlyCloseH - 0.05));
    CHECK(entry_cutoff_h(mk(2026, 12, 24)) == doctest::Approx(kRthEarlyCloseH - 0.05));
    CHECK(entry_cutoff_h(mk(2026, 11, 27)) < kEodBackstopH);
    // And it is still inside the session, so a market order can actually fill.
    CHECK(entry_cutoff_h(mk(2026, 11, 27)) < us_market_close_h(mk(2026, 11, 27)));
}

TEST_CASE("eod backstop: a day the market never opens flattens nothing") {
    // 0 means "do not fire". A weekend session holding an adopted position used
    // to be liquidated at 15:57 on a Saturday, at market, into a shut exchange.
    CHECK(entry_cutoff_h(mk(2026, 8, 15)) == 0.0);   // Saturday
    CHECK(entry_cutoff_h(mk(2026, 8, 16)) == 0.0);   // Sunday
    CHECK(entry_cutoff_h(mk(2026, 11, 26)) == 0.0);  // Thanksgiving
}

TEST_CASE("eod backstop: the engine asks for the day's hour, not a constant") {
    // Source-text pin. The cases above prove the hour is right; only this proves
    // the engine still asks for it. The defect being guarded against is a literal
    // 15.95 in the edge test, which is what shipped for months.
    const std::string path = std::string(TT_REPO_DIR) + "/engine/src/engine.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};

    const size_t gate = src.find("cfg.eod_flatten_h_for_day(tm)");
    REQUIRE(gate != std::string::npos);
    // The crossing test must compare against that resolved hour. Pinned as the
    // whole expression: the edge trigger is load-bearing too (a level trigger
    // turns a restart into a liquidation), so both halves are guarded here.
    CHECK(src.find("if (prev_h < 0.0 || prev_h >= flat_h || hod < flat_h) return;") !=
          std::string::npos);
    // And the old constant must no longer be the thing anything is compared to.
    CHECK(src.find("hod < kEodBackstopH") == std::string::npos);
    CHECK(src.find("prev_h >= kEodBackstopH") == std::string::npos);
}
