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
// ---- the entry gate (LiveConfig::entry_gate) --------------------------------
//
// MAY A STRATEGY OPEN OR ADD TO A POSITION AT THIS LOCAL TIME? Exits are never
// asked; see the LiveConfig::entry_gate comment for why the engine asks the
// wall clock rather than the bar.
//
// It closes EARLIER than the market does, at the same 15:57 the engine's EOD
// backstop flattens on (kEodBackstopH in engine/src/engine.cpp — keep the two
// together). Not an arbitrary margin: the backstop is edge-triggered and marks
// the day done the moment it fires, so a position opened at 15:58 has nothing
// left that will close it today, and a 5-minute strategy's next bar lands after
// the close. Opening a position that nothing will close is the failure class
// that costs the most here, so the last minute one may be OPENED is the last
// minute something will still CLOSE it.
//
// The lead is subtracted from the day's real close so the three 13:00 early
// closes get the same treatment (12:57) rather than a cutoff three hours after
// they shut — which is the bug the half-day arithmetic exists to avoid.
inline constexpr double kEodBackstopH = 15.95;      // 15:57, mirrors the engine
inline constexpr double kEntryCloseLeadH = 0.05;    // 3 minutes

// Last local hour at which an entry may be placed, or 0 when the market does
// not open at all today (weekend/holiday: nothing may be entered).
inline double entry_cutoff_h(const std::tm& tm) {
    const double close_h = us_market_close_h(tm);
    if (close_h <= 0.0) return 0.0;
    return close_h - kEntryCloseLeadH < kEodBackstopH ? close_h - kEntryCloseLeadH
                                                      : kEodBackstopH;
}

// HEALTHY vs BROKEN, since this is a gate and a gate that is always open is
// indistinguishable from no gate at all: true only on a weekday that is not a
// holiday, between 09:30:00 and the cutoff above. False at 09:29:59, false at
// 15:57:00, false all weekend, false on Thanksgiving, false at 04:05 (the hour
// KORU entered on 2026-08-13 and IBKR filled 5.4 hours later at the open), and
// false at 16:00:12 and 16:05:16 (the three orders that started this).
inline bool rth_entry_allowed(const std::tm& tm) {
    const double cutoff = entry_cutoff_h(tm);
    if (cutoff <= 0.0) return false;   // not a trading day
    const int sod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const int open_sod = static_cast<int>(kRthOpenH * 3600.0 + 0.5);
    const int cutoff_sod = static_cast<int>(cutoff * 3600.0 + 0.5);
    return sod >= open_sod && sod < cutoff_sod;
}

// The engine hands nanoseconds; the calendar thinks in local civil time, which
// on the VPS is Eastern by deployment (see the note at the top of this file).
inline bool rth_entry_allowed_at(int64_t now_ns) {
    const std::time_t t = static_cast<std::time_t>(now_ns / 1'000'000'000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return rth_entry_allowed(tm);
}

// ---- the session guard (App::pump_session_guard) ----------------------------
//
// A LIVE SESSION MUST NOT OUTLIVE ITS TRADING DAY. Unconditionally, without
// depending on anything the operator can switch off.
//
// 2026-08-14. /diag at 17:45:50 ET — 1 h 46 m after the close — read
// live_running true, halted false. TradePanel::pump_schedule is supposed to stop
// the session at 15:55, and it did not, because its very first statement is
// `if (!sched_on_) return;` and the VPS config carried "trade_sched_on": false
// (and "trade_sched_stop": "20:00", so even enabled nothing was due). The
// session had been started at 09:41:37 by the daily auto-lineup, which reaches
// start_live_session through lineup_autostart_pending_ and never consults
// sched_on_ at all. It finally ended at 17:46:47 because a human clicked
// "Update & restart".
//
// The app therefore had ONE component that starts unattended sessions
// unconditionally and a DIFFERENT, disabled component as the only thing that
// could ever end one. Every other terminator is a button: sign out, quit,
// update, switch account, kill switch. The engine's own 15:57 EOD backstop
// (kEodBackstopH) flattens and returns — it never clears live_running_ — so it
// cannot end a session either; panels/trade.h already names that as the hazard.
//
// What a session outliving its day costs: entries into a shut exchange (closed
// separately by rth_entry_allowed since 0.23.0, but that gate had not been
// deployed on the 14th), and — still open — holding kTwsOrdersClientId all night
// so the next morning's start collides with our own socket on error 326, which
// is the exact failure the scheduled stop was routed through App::safe_stop_live
// to prevent.
//
// So the rule moves here, where it is pure and testable, and the caller runs it
// from App::tick() independent of the Trade panel's schedule. The panel's
// schedule keeps its job (a stop EARLIER than this, chosen by the operator);
// this is the floor underneath it.

// How long after the close a session is allowed to keep running before the
// guard stops it. Not zero, deliberately: the engine's 15:57 backstop, a
// strategy's own EOD flatten and the last fills of the day all land in the
// minutes around 16:00, and stopping the session on top of them would reap the
// broker adapter mid-flight and lose the fill callbacks — the 2026-08-13 failure
// class. Fifteen minutes is long enough for every close to settle and short
// enough that "the session ended today" is still true.
inline constexpr double kSessionEndLagH = 0.25;   // 15 minutes

// Local hour at which a live session must be stopped today, or 0 when the market
// does not open at all (weekend/holiday: a session running then should stop as
// soon as the guard sees it, which the 0 encodes).
//
// Derived from the DAY's real close, not hardcoded to 16:15 — on the three NYSE
// 1pm early closes a fixed 16:15 would be three hours late, which is the same
// half-day arithmetic bug entry_cutoff_h exists to avoid.
inline double session_end_h(const std::tm& tm) {
    const double close_h = us_market_close_h(tm);
    if (close_h <= 0.0) return 0.0;
    return close_h + kSessionEndLagH;
}

// Should a live session be stopped right now? `last_stop_yday` is the tm_yday
// the guard last fired on (-1 = never), so the stop happens ONCE per day and a
// human who deliberately restarts a session after the close is not fought with
// every tick — the guard is a backstop against a session nobody is watching, not
// a lock on the Start button.
//
// EDGE-TRIGGERED on the crossing, exactly like Engine's own EOD backstop
// (engine.cpp: "A level trigger would flatten the instant the engine came up at,
// say, 16:10"). `prev_sod` is the seconds-of-day this guard last OBSERVED, or -1
// if it has not observed one yet. The guard may only stop a session it watched
// cross the cutoff.
//
// WHY THIS IS NOT OPTIONAL. A level test fires on the first tick of any session
// that starts after the cutoff — which is every VPS deploy, because the evening
// is the only window where deploying is safe (flat book, market closed). The
// first version of this guard paired a level test with a position-liquidating
// stop, so an 18:30 deploy would have cancelled the resting protective orders
// and fired a market flatten into a shut exchange, seconds after coming up.
//
// The non-trading-day arm is gone with it. It read `end_h <= 0 -> true`, which
// stopped any session live on a weekend from the first tick. It is unnecessary:
// a session running unattended into Saturday was already stopped by FRIDAY's
// crossing, so all the arm could still catch is a session a human started
// deliberately on a non-trading day — which is the case the once-per-day rule
// above exists to leave alone, and which cannot trade anyway (the entry gate
// refuses every entry outside RTH).
inline bool session_should_stop(const std::tm& tm, int last_stop_yday, int prev_sod) {
    if (tm.tm_yday == last_stop_yday) return false;   // already stopped today
    if (prev_sod < 0) return false;                   // no crossing observed yet
    const double end_h = session_end_h(tm);
    if (end_h <= 0.0) return false;                   // not a trading day at all
    const int sod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const int end_sod = static_cast<int>(end_h * 3600.0 + 0.5);
    return prev_sod < end_sod && sod >= end_sod;
}

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
