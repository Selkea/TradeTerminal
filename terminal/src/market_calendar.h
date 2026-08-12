#pragma once

// "Is there a US equity open today?" — and, since 0.21.0, "is it open NOW?".
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
// is_us_trading_day answers FULL closures only. NYSE early closes (1pm on
// July 3, the Friday after Thanksgiving, Christmas Eve) are still trading days
// and are deliberately absent from it — the market opens at 09:30 on those, so
// the pre-open check must run. The session-hours half of the file below DOES
// know about them, because "how long has the market been open" is wrong by
// three hours on those days otherwise.
//
// Not covered, and not coverable: ad-hoc closures (national days of mourning,
// weather). Those read as trading days; the cost is one page on a dead day.

#include <cstdint>
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

// ---- session hours ---------------------------------------------------------
//
// Added 0.21.0 for the history-staleness watchdog. On 2026-08-11 a live session
// was left running past the 15:55 auto-stop and the watchdog paged Critical at
// 18:40 and again at 19:10 — "strategies are trading on stale candles" — with
// the market shut since 16:00 and the book flat. It had no notion of hours at
// all, so it would have gone on doing that every 30 minutes until morning.
//
// A page that is wrong every evening is how the page that is right gets
// dismissed, and this particular watchdog exists to catch 2026-08-07, where
// bars sat 4.5-5.2 hours stale during RTH while strategies traded on them.
//
// LOCAL CLOCK = EXCHANGE CLOCK. Every other scheduled thing in this app already
// assumes it (the 08:45-09:15 pre-open window, the 09:25 auto-start, the 15:55
// auto-stop, the 15:57 engine backstop), and the VPS is deployed on Eastern
// time for exactly that reason. Stated once, here, rather than re-derived.
inline constexpr double kRthOpenH = 9.5;          // 09:30
inline constexpr double kRthCloseH = 16.0;        // 16:00
inline constexpr double kRthEarlyCloseH = 13.0;   // 13:00 on the three half-days

// The three NYSE 1pm early closes, each only when it is itself a trading day.
//
// July 3 and December 24 are early closes only when the adjacent holiday has
// not swallowed them: when July 4 falls on a Saturday the NYSE closes ALL DAY
// on Friday July 3, and when Christmas falls on a Saturday it closes all day on
// Friday December 24 — is_us_market_holiday already says so, so ask it rather
// than re-deriving the observance rule. When July 4 falls on a Sunday, July 3
// is a Saturday and the Friday before (July 2) is a normal full session.
//
// The day after Thanksgiving is always the fourth Friday-after-the-fourth-
// Thursday and is never also a holiday, so it needs no such guard.
inline bool is_us_early_close(int y, int m, int d) {
    const int wd = weekday_of(y, m, d);
    if (wd < 1 || wd > 5) return false;                 // weekend: nothing opens
    if (is_us_market_holiday(y, m, d)) return false;    // fully shut, not early
    if (m == 7 && d == 3) return true;
    if (m == 12 && d == 24) return true;
    return m == 11 && d == nth_weekday(y, 11, 4, 4) + 1;   // day after Thanksgiving
}

// Local hour at which the regular session ends on this date, or 0 when the
// market does not open at all.
inline double us_market_close_h(const std::tm& tm) {
    if (!is_us_trading_day(tm)) return 0.0;
    return is_us_early_close(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)
               ? kRthEarlyCloseH
               : kRthCloseH;
}

// How long the REGULAR session has been open at local time `tm`, in ms — 0 on a
// weekend, a holiday, before 09:30, and at or after the close.
//
// The single call the watchdog gating is built on, for two reasons rolled into
// one number:
//
//   1. It is the gate. 0 means "nothing the market does can be late right now",
//      which is the whole 2026-08-11 fix.
//   2. It is what the settle-in window at the open is measured off, which a
//      bare is-it-open boolean would NOT give. The terminal is meant to survive
//      the nightly stop/start, but a session that was never stopped carries
//      yesterday's deliveries into this morning: at 09:30:01 a symbol last
//      served at 17:00 yesterday is sixteen hours "stale" and would page on the
//      first frame the gate opened. The watchdog waits net::kHistArmSettleMs of
//      this figure before judging anything — a FIXED 45 minutes, not a cap on
//      each symbol's age, which is the distinction 0.21.0 got wrong: a cap
//      cannot exceed the 389 minutes a full session offers, so it silently
//      un-watched every symbol whose grace was longer than that.
//
// Deliberately NOT clamped to a session's own start: the caller owns that (it
// takes the min of the two), because "how long has the market been open" is a
// property of the day, not of the app.
// Integer seconds-of-day throughout, deliberately: the obvious
// (hod - 9.5) * 3600 * 1000 in double loses a millisecond to representation at
// 15:59 and turns an exact minute count into 23339999. Nothing here depends on
// that millisecond, but a boundary function whose value is one short of the
// round number is a trap for the next person to write a test against it.
inline int64_t rth_open_elapsed_ms(const std::tm& tm) {
    const double close_h = us_market_close_h(tm);
    if (close_h <= 0.0) return 0;
    const int sod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const int open_sod = static_cast<int>(kRthOpenH * 3600.0 + 0.5);
    const int close_sod = static_cast<int>(close_h * 3600.0 + 0.5);
    if (sod < open_sod || sod >= close_sod) return 0;
    return static_cast<int64_t>(sod - open_sod) * 1000;
}

} // namespace tt
