#pragma once

// "Is there a US equity open today?" — the smallest calendar that answers it.
//
// Written for the pre-open gateway check (see App::pump_preopen_gateway_check).
// That check does not merely page: on a paper account it KILLS AND RELAUNCHES
// IB Gateway, which spends a login attempt, and IBKR locks accounts on repeated
// failed logins. A weekday test alone would spend one of those every
// Thanksgiving and every Good Friday — days with no open to be pre- of, and
// days on which IBKR runs an extended maintenance window, which is the very
// thing that returns the misleading "UNRECOGNIZED USERNAME OR PASSWORD" that
// cost 13 hours on 2026-08-09. Doing nothing on a holiday is free; doing
// something is not.
//
// Full closures only. NYSE early closes (1pm on July 3, the Friday after
// Thanksgiving, Christmas Eve) are still trading days and deliberately absent —
// the market opens at 09:30 on those, so the pre-open check must run.
//
// Not covered, and not coverable: ad-hoc closures (national days of mourning,
// weather). Those read as trading days; the cost is one page on a dead day.

#include <ctime>

namespace tt {

// Days since 1970-01-01 for a civil date. Howard Hinnant's days_from_civil,
// valid for any Gregorian date; used only to get weekdays here.
inline long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

// 0 = Sunday .. 6 = Saturday, matching std::tm::tm_wday. 1970-01-01 was a
// Thursday (4).
inline int weekday_of(int y, int m, int d) {
    const long long z = days_from_civil(y, m, d);
    return static_cast<int>(((z % 7) + 11) % 7);   // +11 == +4 mod 7, kept positive
}

// Day-of-month of the nth `wd` in a month (n is 1-based).
inline int nth_weekday(int y, int m, int wd, int n) {
    const int first = weekday_of(y, m, 1);
    return 1 + ((wd - first + 7) % 7) + (n - 1) * 7;
}

// Day-of-month of the last `wd` in a month of `len` days.
inline int last_weekday(int y, int m, int wd, int len) {
    return len - ((weekday_of(y, m, len) - wd + 7) % 7);
}

// Good Friday = Easter Sunday - 2. Easter by the Anonymous Gregorian algorithm;
// it lands between March 22 and April 25, so Good Friday is March 20 - April 23.
inline void good_friday(int y, int& mon, int& day) {
    const int a = y % 19, b = y / 100, c = y % 100;
    const int d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
    const int h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4, k = c % 4;
    const int l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    const int emon = (h + l - 7 * m + 114) / 31;    // 3 = March, 4 = April
    const int eday = ((h + l - 7 * m + 114) % 31) + 1;
    if (eday > 2) { mon = emon; day = eday - 2; return; }
    // Easter on April 1 or 2 -> Good Friday in March.
    mon = 3;
    day = 31 + eday - 2;
}

// A full NYSE/Nasdaq closure. Month is 1-12.
inline bool is_us_market_holiday(int y, int m, int d) {
    // Fixed-date holidays, with the NYSE observance rule: a Saturday holiday
    // moves BACK to the Friday, a Sunday holiday moves FORWARD to the Monday.
    // The one exception is New Year's Day on a Saturday, which is not observed
    // at all — Dec 31 stays a trading day.
    static constexpr int kFixed[][2] = {{1, 1}, {6, 19}, {7, 4}, {12, 25}};
    for (const auto& f : kFixed) {
        const int fm = f[0], fd = f[1];
        const int wd = weekday_of(y, fm, fd);
        int om = fm, od = fd;
        if (wd == 6) {                                  // Saturday
            if (fm == 1 && fd == 1) continue;           // New Year's: not observed
            od = fd - 1;                                // ...none of the others is
        } else if (wd == 0) {                           // Sunday -> Monday
            od = fd + 1;                                // (Jun 20 / Jul 5 / Dec 26 /
        }                                               //  Jan 2 all stay in-month)
        if (m == om && d == od) return true;
    }
    if (m == 1 && d == nth_weekday(y, 1, 1, 3)) return true;    // MLK, 3rd Monday
    if (m == 2 && d == nth_weekday(y, 2, 1, 3)) return true;    // Washington, 3rd Mon
    if (m == 5 && d == last_weekday(y, 5, 1, 31)) return true;  // Memorial, last Mon
    if (m == 9 && d == nth_weekday(y, 9, 1, 1)) return true;    // Labor, 1st Monday
    if (m == 11 && d == nth_weekday(y, 11, 4, 4)) return true;  // Thanksgiving, 4th Thu
    int gm = 0, gd = 0;
    good_friday(y, gm, gd);
    return m == gm && d == gd;
}

// The question callers actually ask, straight off a localtime_s() result.
inline bool is_us_trading_day(const std::tm& tm) {
    if (tm.tm_wday < 1 || tm.tm_wday > 5) return false;
    return !is_us_market_holiday(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

} // namespace tt
