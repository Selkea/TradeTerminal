// US market calendar. Only one thing depends on it — whether the 08:45 pre-open
// gateway check runs — but that check kills and relaunches IB Gateway, which
// spends a login attempt on an account IBKR locks after repeated failures. A
// wrong "yes" on Thanksgiving spends one for nothing, into IBKR's holiday
// maintenance window; a wrong "no" on a real trading day puts back the 13-hour
// blindness of 2026-08-09. So both directions are pinned here.
#include "doctest.h"

#include "market_calendar.h"

using namespace tt;

// A std::tm the way localtime_s fills one in, so the tests drive the exact
// entry point App::pump_preopen_gateway_check calls.
static std::tm mk(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_wday = weekday_of(y, m, d);
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
