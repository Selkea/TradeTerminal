// The scheduled daily-lineup trigger (lineup_schedule.h).
//
// Until 0.53.0 the day was marked "built" on the first tick inside the
// 09:35-10:05 window, BEFORE start_daily_lineup could refuse - so a data session
// that happened to be down at 09:35 cost the whole trading day, silently. Every
// case below plays a whole day through the same decide -> record -> say sequence
// App::pump_lineup_schedule runs, two ticks per minute (the app ticks per frame;
// a rule that only holds at one tick per minute is not the rule).
#include "doctest.h"

#include "alert_rules.h"
#include "lineup_schedule.h"

#include <functional>
#include <string>
#include <vector>

using tt::ui::AlertCategory;
using tt::ui::AlertClass;
using tt::ui::classify_alert;
using tt::ui::classify_alert_category;
using tt::ui::kLineupWindowMin;
using tt::ui::LineupBlock;
using tt::ui::LineupLatch;
using tt::ui::LineupTick;
using tt::ui::lineup_missed_line;
using tt::ui::lineup_schedule_decide;
using tt::ui::lineup_schedule_record;
using tt::ui::lineup_wait_line;

namespace {

constexpr int kBuild = 9 * 60 + 35;   // 09:35, the VPS's configured build time
constexpr int kOpen = 8 * 60;         // when the simulated app is already running
constexpr int kEnd = 16 * 60 + 30;

struct DayRun {
    int builds = 0;           // builds that BEGAN
    int attempts = 0;         // Build decisions, began or not
    int first_build_min = -1;
    int missed = 0;
    std::vector<std::string> said;   // the operator lines (wait / missed)
};

// The same sequence as App::pump_lineup_schedule, verbatim in shape.
DayRun run_day(LineupLatch& latch, int yday, bool trading_day, int from_min, int to_min,
               const std::function<LineupBlock(int)>& block_at,
               const std::function<bool(int)>& start_ok = [](int) { return true; },
               bool tws_route = true) {
    DayRun r;
    for (int m = from_min; m <= to_min; ++m) {
        for (int frame = 0; frame < 2; ++frame) {
            const LineupBlock b = block_at(m);
            const LineupTick t = lineup_schedule_decide(trading_day, yday, m, kBuild, latch, b);
            if (t == LineupTick::Skip) continue;
            if (t == LineupTick::Build) {
                ++r.attempts;
                const bool began = start_ok(m);
                if (began) {
                    ++r.builds;
                    if (r.first_build_min < 0) r.first_build_min = m;
                }
                lineup_schedule_record(latch, t, yday, b, began || !tws_route);
                continue;
            }
            if (!lineup_schedule_record(latch, t, yday, b, false)) continue;
            if (t == LineupTick::Missed) ++r.missed;
            r.said.push_back(t == LineupTick::Wait ? lineup_wait_line(kBuild, b)
                                                   : lineup_missed_line(kBuild, latch.waited_why));
        }
    }
    return r;
}

LineupBlock always(LineupBlock b, int) { return b; }

} // namespace

TEST_CASE("lineup schedule: data down at 09:35, back at 09:40 -> the day still builds") {
    // THE BUG. The old code marked the day on the 09:35 tick, the build refused
    // on the down data session, and 09:40 found the day already marked: zero
    // builds, no session, nothing said anywhere the operator looks.
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd, [](int m) {
        return m < kBuild + 5 ? LineupBlock::DataDown : LineupBlock::None;
    });
    CHECK(r.builds == 1);
    CHECK(r.first_build_min == kBuild + 5);
    CHECK(r.missed == 0);
    // Said ONCE, though the app sat in Wait for ten ticks.
    REQUIRE(r.said.size() == 1);
    CHECK(r.said[0].find("waiting") != std::string::npos);
    CHECK(r.said[0].find("data session") != std::string::npos);
}

TEST_CASE("lineup schedule: data down for the whole window -> one wait, one miss, no build") {
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd,
                             [](int m) { return always(LineupBlock::DataDown, m); });
    CHECK(r.builds == 0);
    CHECK(r.attempts == 0);
    CHECK(r.missed == 1);   // once, though ticks run on to 16:30
    REQUIRE(r.said.size() == 2);
    CHECK(r.said[1].find("lineup: ABORTED") == 0);
    CHECK(r.said[1].find("data session") != std::string::npos);
    CHECK(r.said[1].find("09:35-10:05") != std::string::npos);
}

TEST_CASE("lineup schedule: the miss is reported the first minute AFTER the window") {
    LineupLatch latch;
    int missed_at = -1;
    for (int m = kOpen; m <= kEnd && missed_at < 0; ++m) {
        const LineupTick t = lineup_schedule_decide(true, 300, m, kBuild, latch,
                                                    LineupBlock::DataDown);
        if (t == LineupTick::Missed) missed_at = m;
        lineup_schedule_record(latch, t, 300, LineupBlock::DataDown, false);
    }
    CHECK(missed_at == kBuild + kLineupWindowMin + 1);   // 10:06, not 10:05
}

TEST_CASE("lineup schedule: an optimizer busy all window is waited out, then reported") {
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd,
                             [](int m) { return always(LineupBlock::OptimizerBusy, m); });
    CHECK(r.builds == 0);
    REQUIRE(r.missed == 1);
    CHECK(r.said.back().find("optimizer was busy") != std::string::npos);
}

TEST_CASE("lineup schedule: a session already trading is never a missed day") {
    // The operator pressed Start at 09:31. No build, no wait, and above all no
    // "no session today" page about a day that is trading.
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd, [](int m) {
        return m >= 9 * 60 + 31 ? LineupBlock::AlreadyTrading : LineupBlock::None;
    });
    CHECK(r.attempts == 0);
    CHECK(r.missed == 0);
    CHECK(r.said.empty());
}

TEST_CASE("lineup schedule: an app started after the window pages nothing") {
    // A deploy at 17:00, or a restart at 11:00: this process never waited in the
    // window, and may have been preceded by one that built - it must not page.
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, 11 * 60, kEnd,
                             [](int m) { return always(LineupBlock::None, m); });
    CHECK(r.attempts == 0);
    CHECK(r.missed == 0);
    CHECK(r.said.empty());
}

TEST_CASE("lineup schedule: not a trading day -> nothing at all") {
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, false, kOpen, kEnd,
                             [](int m) { return always(LineupBlock::DataDown, m); });
    CHECK(r.attempts == 0);
    CHECK(r.missed == 0);
    CHECK(r.said.empty());
}

TEST_CASE("lineup schedule: a refused start leaves the day unmarked, and retries") {
    // The one refusal `block` cannot foresee: the data session dropping in the
    // instant between the check and the call. The day must stay open.
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd,
                             [](int m) { return always(LineupBlock::None, m); },
                             [](int m) { return m >= kBuild + 2; });
    CHECK(r.builds == 1);
    CHECK(r.first_build_min == kBuild + 2);
    CHECK(r.attempts > 1);
    CHECK(latch.built_day == 300);
}

TEST_CASE("lineup schedule: the web route is marked on its first attempt, not retried all window") {
    LineupLatch latch;
    const DayRun r = run_day(latch, 300, true, kOpen, kEnd,
                             [](int m) { return always(LineupBlock::None, m); },
                             [](int) { return false; }, /*tws_route=*/false);
    CHECK(r.attempts == 1);
    CHECK(r.builds == 0);
    CHECK(latch.built_day == 300);
}

TEST_CASE("lineup schedule: built once per day, and again the next trading day") {
    LineupLatch latch;
    const auto up = [](int m) { return always(LineupBlock::None, m); };
    const DayRun d1 = run_day(latch, 300, true, kOpen, kEnd, up);
    const DayRun d2 = run_day(latch, 301, true, kOpen, kEnd, up);
    CHECK(d1.builds == 1);
    CHECK(d1.attempts == 1);
    CHECK(d2.builds == 1);
    CHECK(d2.first_build_min == kBuild);
}

TEST_CASE("lineup schedule: window edges - 09:35 and 10:05 are in, 10:06 is out") {
    LineupLatch none;
    CHECK(lineup_schedule_decide(true, 1, kBuild - 1, kBuild, none, LineupBlock::None) ==
          LineupTick::Skip);
    CHECK(lineup_schedule_decide(true, 1, kBuild, kBuild, none, LineupBlock::None) ==
          LineupTick::Build);
    CHECK(lineup_schedule_decide(true, 1, kBuild + kLineupWindowMin, kBuild, none,
                                 LineupBlock::None) == LineupTick::Build);
    CHECK(lineup_schedule_decide(true, 1, kBuild + kLineupWindowMin + 1, kBuild, none,
                                 LineupBlock::None) == LineupTick::Skip);
}

TEST_CASE("lineup schedule: the wait line is quiet, the miss pages Critical under System") {
    const std::string wait = lineup_wait_line(kBuild, LineupBlock::DataDown);
    CHECK(classify_alert(wait) == AlertClass::None);
    CHECK(wait.find("09:35") != std::string::npos);
    CHECK(wait.find("10:05") != std::string::npos);

    const std::string miss = lineup_missed_line(kBuild, LineupBlock::DataDown);
    CHECK(classify_alert(miss) == AlertClass::Critical);
    // System, NOT Lineup: the VPS has Lineup notifications switched off, and the
    // category gate drops a disabled category's alerts whatever their severity.
    CHECK(classify_alert_category(miss) == AlertCategory::System);
}

TEST_CASE("alert categories: a refused trading day is System, routine lineup news stays Lineup") {
    // The lineup's existing refused-day verdict, verbatim in shape.
    CHECK(classify_alert_category("lineup: ABORTED - not one of 6 picks produced a usable "
                                  "parameter set. Starting NOTHING rather than trading on "
                                  "another symbol's parameters") == AlertCategory::System);
    // Answered before Connection: a verdict that mentions the gateway is still a
    // refused day, not a connection blip.
    CHECK(classify_alert_category("lineup: ABORTED - the gateway was down all window") ==
          AlertCategory::System);
    CHECK(classify_alert_category("lineup: EXCLUDED MSTZ, KORU - no fit") ==
          AlertCategory::Lineup);
    CHECK(classify_alert_category("lineup: scanning IBKR for high-volatility movers...") ==
          AlertCategory::Lineup);
}
