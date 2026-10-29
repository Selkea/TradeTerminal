#pragma once
// The scheduled daily-lineup trigger, as a pure decision.
//
// WHY THIS IS ITS OWN HEADER. Until 0.53.0 this logic lived inline in
// App::pump_lineup_schedule, which nothing in the test suite can construct, and
// it carried a bug that could cost a whole trading day without a word to anyone:
//
//     lineup_last_build_day_ = tm.tm_yday;              // day marked as BUILT
//     ...
//     start_daily_lineup(...);                          // ...which can refuse:
//         if (!data_.connected()) { "lineup: data session not connected"; return; }
//
// The day was marked on the first tick inside the 09:35-10:05 window, BEFORE the
// build was attempted. If the TWS data session happened to be down at that
// instant - an app restart near 09:35, a gateway relaunched by the 08:45 check
// still logging in, a data client mid-reconnect - the build refused, the day
// was already marked, and nothing retried. On a box where the lineup is the only
// thing that starts a session, that is the day gone. And it was silent twice
// over: the refusal line is routed to optimizer.log (route() files every
// "lineup:" line there), and the one verdict that does page - "lineup: ABORTED"
// - was filed under the Lineup alert category, which the VPS has switched off.
//
// Same lesson as most of this project's worst bugs: a latch named for one thing
// ("one build per day") was set by another (one ATTEMPT per day).
//
// Found 2026-10-29 by an audit of what the app needs from its host, not by an
// incident - it has not been seen to fire, and optimizer.log keeps too little
// history to say whether it ever did.

#include <cstdio>
#include <string>

namespace tt::ui {

// How long after the configured build time a scheduled build may still start.
inline constexpr int kLineupWindowMin = 30;

// Why a build cannot start right now, if it cannot. These mirror, one for one,
// the refusals at the top of App::start_daily_lineup, so a Build decision is one
// that call will honour.
enum class LineupBlock {
    None,             // free to build
    DataDown,         // the TWS data session is not connected
    OptimizerBusy,    // a tournament or sweep is running
    AlreadyTrading,   // a live session, or a lineup build, is already under way
};

// What App::pump_lineup_schedule should do on this tick.
enum class LineupTick {
    Skip,     // nothing to do
    Wait,     // inside the window, cannot start yet: try again next tick
    Build,    // start the build now
    Missed,   // the window closed while this process waited: say so, once
};

// The day markers, each set only by the event it is named for.
struct LineupLatch {
    int built_day = -1;                         // tm_yday a build actually BEGAN
    int waited_day = -1;                        // tm_yday this process waited in the window
    LineupBlock waited_why = LineupBlock::None; // the most recent reason it waited
    int missed_day = -1;                        // tm_yday the missed window was reported
};

inline LineupTick lineup_schedule_decide(bool trading_day, int yday, int now_min,
                                         int build_min, const LineupLatch& latch,
                                         LineupBlock block) {
    if (!trading_day) return LineupTick::Skip;
    if (yday == latch.built_day) return LineupTick::Skip;   // one BUILD per day
    // A session or a build already running is not a missed day - something is
    // trading or about to - so it never waits and never reports a miss.
    if (block == LineupBlock::AlreadyTrading) return LineupTick::Skip;
    if (now_min < build_min) return LineupTick::Skip;
    if (now_min > build_min + kLineupWindowMin) {
        // Only a process that was HERE, waiting, may report the miss. One
        // started after the window never saw it: a deploy at 17:00 must not page
        // "no build today" about a day whose build ran in the previous process.
        return latch.waited_day == yday && latch.missed_day != yday ? LineupTick::Missed
                                                                    : LineupTick::Skip;
    }
    return block == LineupBlock::None ? LineupTick::Build : LineupTick::Wait;
}

// Records what a tick did, and says whether that tick's line should be logged.
// Kept here rather than in App so the two rules that matter are testable:
//
//   - a Wait is said ONCE per day, not once per frame for up to 30 minutes;
//   - the day is marked built only when `mark_day` - when start_daily_lineup
//     actually BEGAN a build (or the route can never build at all). Marking it
//     on the attempt is precisely the bug this header exists to remove.
inline bool lineup_schedule_record(LineupLatch& l, LineupTick t, int yday,
                                   LineupBlock block, bool mark_day) {
    switch (t) {
    case LineupTick::Wait: {
        l.waited_why = block;
        const bool first_today = l.waited_day != yday;
        l.waited_day = yday;
        return first_today;
    }
    case LineupTick::Missed:
        l.missed_day = yday;
        return true;
    case LineupTick::Build:
        if (mark_day) l.built_day = yday;
        return true;
    default:
        return false;
    }
}

inline std::string lineup_hhmm(int min_of_day) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", (min_of_day / 60) % 24, min_of_day % 60);
    return b;
}

inline const char* lineup_block_reason(LineupBlock why) {
    switch (why) {
    case LineupBlock::DataDown:      return "the data session is not connected";
    case LineupBlock::OptimizerBusy: return "the optimizer is busy";
    default:                         return "it cannot start yet";
    }
}

// Logged once per day when the window opens and the build cannot start. QUIET
// on purpose - ordinary lineup prose pages nobody (classify_alert) - because it
// may well resolve itself a minute later.
inline std::string lineup_wait_line(int build_min, LineupBlock why) {
    return "lineup: scheduled build (" + lineup_hhmm(build_min) + ") is waiting - " +
           lineup_block_reason(why) + "; retrying until " +
           lineup_hhmm(build_min + kLineupWindowMin);
}

// The verdict when the window closes unbuilt. Carries "lineup: ABORTED", the
// same tag as the lineup's other refused-trading-day verdict, so it pages
// Critical - and that tag is filed under System, not Lineup (alert_rules.h), so
// switching off routine lineup news cannot also silence "no session today".
inline std::string lineup_missed_line(int build_min, LineupBlock why) {
    const char* reason = why == LineupBlock::OptimizerBusy
                             ? "the optimizer was busy for the whole window"
                             : "the data session was not connected at any point in it";
    return "lineup: ABORTED - no scheduled build today: " + std::string(reason) +
           " (" + lineup_hhmm(build_min) + "-" + lineup_hhmm(build_min + kLineupWindowMin) +
           "), so no session will start on its own. Start one from the Trade panel "
           "once that is resolved.";
}

} // namespace tt::ui
