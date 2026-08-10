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
