// The tick loop's pacing, its clock, and the watchdog timer that clock feeds.
//
// This is the testable half of the 2026-08-11 iconify freeze. The main loop
// itself is not unit-testable — it needs a window, a GL context and an ImGui
// backend — but the two things that were actually wrong are:
//
//   1. the app's deadlines ran on ImGui::GetTime(), a clock that only advances
//      when a frame is built, and
//   2. nothing paced the loop when no frame was built.
//
// Both are arithmetic once the clock is a parameter, so both are pinned here.
// A FrameClock below stands in for ImGui::GetTime(): it advances ONLY when
// new_frame() is called, exactly like ImGui_ImplGlfw_NewFrame accumulating
// io.DeltaTime — including the unclamped catch-up jump on the first frame after
// a minimize. Driving the real WatchdogTimer off it reproduces the incident.
#include "doctest.h"

#include "block_wait.h"
#include "panels/sweep.h"   // kSweepDrainBudgetMs — the budget the drain's wait must fit
#include "tick_clock.h"
#include "watchdog_timer.h"

#include <chrono>
#include <thread>

using namespace tt::ui;
using Action = WatchdogTimer::Action;

// ---------------------------------------------------------------------------
// Pacing policy
// ---------------------------------------------------------------------------

TEST_CASE("tick_sleep_ms paces the not-rendering loop to a fixed period") {
    // An idle tick returns almost immediately, so nearly the whole interval is
    // slept away: this is the case that must not spin a core.
    CHECK(tick_sleep_ms(0) == kTickIntervalMs);
    CHECK(tick_sleep_ms(1) == kTickIntervalMs - 1);

    // A PERIOD, not a delay. A tick that used part of its slot sleeps only the
    // remainder, so the loop's cadence does not drift with the work it does.
    CHECK(tick_sleep_ms(kTickIntervalMs - 1) == 1);
    CHECK(tick_sleep_ms(kTickIntervalMs / 2) == kTickIntervalMs - kTickIntervalMs / 2);

    // A tick that overran goes straight into the next one rather than adding a
    // full sleep on top — otherwise the busiest ticks, which are the ones with
    // work waiting (the optimizer's 8 ms result drain), would be paced hardest.
    CHECK(tick_sleep_ms(kTickIntervalMs) == 0);
    CHECK(tick_sleep_ms(kTickIntervalMs + 1) == 0);
    CHECK(tick_sleep_ms(5000) == 0);

    // ...and it can never go NEGATIVE, which the host would pass to a sleep as
    // an unsigned duration.
    CHECK(tick_sleep_ms(1'000'000) >= 0);
}

TEST_CASE("the tick interval is fine enough for what the pumps need") {
    // The optimizer's per-call drain budget (kSweepDrainBudgetMs = 8 ms, see
    // panels/sweep.h). A tick period at or below the rendered loop's ~33 ms is
    // what keeps a hidden window from optimizing slower than a visible one; the
    // hard requirement is only that a drain gets a meaningful share of each
    // period, i.e. the period is not an order of magnitude larger than the drain.
    CHECK(kTickIntervalMs <= 33);
    // Finer than the tightest TIMER any pump has: pump_diag rebuilds /diag at
    // 1 Hz and everything else (watchdog grace periods, lineup phase deadlines,
    // the minute-resolution session schedule) is coarser still.
    CHECK(kTickIntervalMs <= 1000);
    // But not so fine that the loop becomes a spin. 10 ms is what the old
    // iconified branch already slept.
    CHECK(kTickIntervalMs >= 5);
}

TEST_CASE("the drain's wait blocks instead of spinning") {
    // The other half of "the not-rendering loop costs no more CPU than the code
    // it replaced". Pacing the LOOP is useless if a pump inside it spins, and
    // App::pump_sweep's result drain waits on an in-flight backtest for most of
    // its 8 ms budget on every tick a sweep is running.
    //
    // The trap this pins: std::this_thread::sleep_for does not sleep at all
    // below 1 ms on this toolchain (mingw-w64 truncates nanosleep to whole
    // milliseconds), so the sleep_for(250us) the drain used to wait on returned
    // in ~27 ns and burned 74% of a core once tick() started calling it every
    // 10 ms. See block_wait.h. Both checks below fail on that call and pass on
    // wait_off_core().

    // It blocks: eight waits cannot pass in the time a spin would take.
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 8; ++i) wait_off_core();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(us >= 4000);   // >= 0.5 ms each; the spin managed ~27 ns each
    }

    // And the property the drain actually depends on: one full drain budget of
    // waiting costs a handful of iterations, not hundreds of thousands. The
    // bound is deliberately loose (a spin hit ~263,500 per 8 ms, four hundred
    // times this ceiling) so the test measures the defect, not the scheduler.
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto spent_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
        };
        long long iters = 0;
        while (spent_ms() < kSweepDrainBudgetMs) {
            ++iters;
            wait_off_core();
        }
        CHECK(iters > 0);
        CHECK(iters < 1000);
    }
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

TEST_CASE("MonoClock advances with no frames at all") {
    // The whole point: not one NewFrame happens in this test, and the clock
    // still moves. ImGui::GetTime() would have returned the same value twice.
    const MonoClock clk;
    const double t0 = clk.now_s();
    const int64_t m0 = clk.now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const double t1 = clk.now_s();
    const int64_t m1 = clk.now_ms();

    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 0.015);   // generous: Windows sleeps are granular
    CHECK(t1 - t0 < 5.0);      // ...but it is seconds, not milliseconds or ticks
    CHECK(m1 >= m0 + 15);
    CHECK(m1 < m0 + 5000);
    // The two readings are the same clock in different units.
    CHECK(static_cast<double>(m1) / 1000.0 == doctest::Approx(t1).epsilon(0.05));
}

TEST_CASE("MonoClock is monotonic") {
    const MonoClock clk;
    double prev = clk.now_s();
    for (int i = 0; i < 200; ++i) {
        const double now = clk.now_s();
        CHECK(now >= prev);
        prev = now;
    }
    CHECK(prev >= 0.0);   // measured from construction, never negative
}

// ---------------------------------------------------------------------------
// The watchdog timer
// ---------------------------------------------------------------------------

TEST_CASE("WatchdogTimer stays quiet through the grace period") {
    WatchdogTimer wd;
    CHECK_FALSE(wd.in_episode());
    CHECK(wd.update(true, 100.0, 60.0, 900.0) == Action::None);   // episode starts
    CHECK(wd.in_episode());
    CHECK(wd.age_s(130.0) == doctest::Approx(30.0));
    CHECK(wd.update(true, 130.0, 60.0, 900.0) == Action::None);
    CHECK(wd.update(true, 159.9, 60.0, 900.0) == Action::None);
    CHECK_FALSE(wd.alerted());
}

// t = 0.0 is a REAL time on mono_s() — it is App construction, not "unset". A
// timestamp-sentinel implementation reads the first instants of the process as
// "no episode in progress" and can never page about them, so an outage present
// at startup would be invisible until it cleared and came back.
TEST_CASE("WatchdogTimer works at t = 0") {
    WatchdogTimer wd;
    CHECK(wd.update(true, 0.0, 60.0, 900.0) == Action::None);
    CHECK(wd.in_episode());
    CHECK(wd.age_s(0.0) == doctest::Approx(0.0));
    CHECK(wd.update(true, 59.0, 60.0, 900.0) == Action::None);
    CHECK(wd.update(true, 60.0, 60.0, 900.0) == Action::Page);
    CHECK(wd.age_s(60.0) == doctest::Approx(60.0));
    // ...and the page's own timestamp of 0.0 must not read as "never paged".
    WatchdogTimer at_zero;
    CHECK(at_zero.update(true, 0.0, 0.0, 900.0) == Action::Page);
    CHECK(at_zero.alerted());
    CHECK(at_zero.update(true, 1.0, 0.0, 900.0) == Action::None);   // inside re-alert
    CHECK(at_zero.update(false, 2.0, 0.0, 900.0) == Action::Recovered);
}

TEST_CASE("WatchdogTimer pages once the bad state outlasts its grace") {
    WatchdogTimer wd;
    CHECK(wd.update(true, 100.0, 60.0, 900.0) == Action::None);
    CHECK(wd.update(true, 160.0, 60.0, 900.0) == Action::Page);
    CHECK(wd.alerted());
    // The alert text quotes the episode's age, so a Page must leave the episode
    // anchored at its START rather than restarting it at the moment we paged.
    CHECK(wd.in_episode());
    CHECK(wd.age_s(160.0) == doctest::Approx(60.0));
    // No re-page inside the re-alert gap, however many times it is polled — and
    // it IS polled ~100x a second by the tick loop.
    for (double t = 161.0; t < 1060.0; t += 1.0)
        CHECK(wd.update(true, t, 60.0, 900.0) == Action::None);
    CHECK(wd.update(true, 1060.0, 60.0, 900.0) == Action::Page);
}

TEST_CASE("WatchdogTimer announces recovery only to whoever heard the page") {
    WatchdogTimer paged;
    CHECK(paged.update(true, 0.0, 60.0, 900.0) == Action::None);
    CHECK(paged.update(true, 61.0, 60.0, 900.0) == Action::Page);
    CHECK(paged.update(false, 62.0, 60.0, 900.0) == Action::Recovered);
    CHECK_FALSE(paged.in_episode());
    CHECK_FALSE(paged.alerted());
    // ...and only once.
    CHECK(paged.update(false, 63.0, 60.0, 900.0) == Action::None);

    // A blip that cleared inside the grace was never worth a page, so its all
    // clear is not worth one either. A nightly 1100->1102 gateway blip is this.
    WatchdogTimer blip;
    CHECK(blip.update(true, 0.0, 60.0, 900.0) == Action::None);
    CHECK(blip.update(false, 5.0, 60.0, 900.0) == Action::None);
    CHECK_FALSE(blip.in_episode());

    // A second episode after a recovery is a fresh grace period, not a resumption.
    CHECK(blip.update(true, 100.0, 60.0, 900.0) == Action::None);
    CHECK(blip.age_s(100.0) == doctest::Approx(0.0));
    CHECK(blip.update(true, 159.0, 60.0, 900.0) == Action::None);
    CHECK(blip.update(true, 161.0, 60.0, 900.0) == Action::Page);
}

TEST_CASE("WatchdogTimer::reset stands down silently") {
    WatchdogTimer wd;
    CHECK(wd.update(true, 0.0, 60.0, 900.0) == Action::None);
    CHECK(wd.update(true, 61.0, 60.0, 900.0) == Action::Page);
    wd.reset();
    CHECK_FALSE(wd.in_episode());
    CHECK_FALSE(wd.alerted());
    // Nothing is emitted by reset itself: a session ending must not send the
    // operator a "broker reconnected" every afternoon.
    // The next bad reading starts a brand-new episode with a full grace.
    CHECK(wd.update(true, 100.0, 60.0, 900.0) == Action::None);
    CHECK(wd.age_s(159.0) == doctest::Approx(59.0));
    CHECK(wd.update(true, 159.0, 60.0, 900.0) == Action::None);
}

// ---------------------------------------------------------------------------
// The regression: the clock is the bug
// ---------------------------------------------------------------------------

// ImGui's clock. It advances only when a frame is built, and its catch-up on the
// first frame after a gap is UNCLAMPED — ImGui_ImplGlfw_NewFrame sets
// io.DeltaTime = glfwGetTime() - last, so the whole minimized interval lands in
// one step. Both halves matter and both are exercised below.
namespace {
class FrameClock {
public:
    // `wall_s` is real time; the clock only samples it when a frame is drawn.
    void new_frame(double wall_s) { t_ = wall_s; }
    double now_s() const { return t_; }

private:
    double t_ = 0.0;
};
} // namespace

TEST_CASE("a frame-driven clock never pages a real outage while no frame is drawn") {
    // The incident, replayed. The broker goes down at t=100 and stays down. The
    // window is minimized at t=105, so no frame is built until t=3705 (one hour).
    // The watchdog is polled all along — it is the CLOCK that is frozen, not the
    // call — which is precisely why the freeze was silent.
    FrameClock frame;
    WatchdogTimer wd;

    frame.new_frame(100.0);
    CHECK(wd.update(true, frame.now_s(), 60.0, 900.0) == Action::None);
    frame.new_frame(105.0);
    CHECK(wd.update(true, frame.now_s(), 60.0, 900.0) == Action::None);

    // Minimized. The host keeps polling every 10 ms for an hour; no NewFrame, so
    // no clock movement. A 60 s grace is never crossed by an hour of downtime.
    for (int i = 0; i < 360'000; ++i)
        REQUIRE(wd.update(true, frame.now_s(), 60.0, 900.0) == Action::None);
    CHECK_FALSE(wd.alerted());

    // Restored at t=3705: ImGui's clock catches up in ONE unclamped step, and the
    // page finally goes out reporting a down-time it never actually measured —
    // 3605 s of "outage" attributed to a single frame.
    frame.new_frame(3705.0);
    CHECK(wd.update(true, frame.now_s(), 60.0, 900.0) == Action::Page);
    CHECK(wd.age_s(frame.now_s()) == doctest::Approx(3605.0));

    // The same hour on a steady clock: paged inside the grace period, an hour
    // earlier, which is the entire purpose of a 60 s threshold.
    WatchdogTimer steady;
    CHECK(steady.update(true, 100.0, 60.0, 900.0) == Action::None);
    CHECK(steady.update(true, 105.0, 60.0, 900.0) == Action::None);
    CHECK(steady.update(true, 160.1, 60.0, 900.0) == Action::Page);
    CHECK(steady.age_s(160.1) == doctest::Approx(60.1));
}

TEST_CASE("a frozen clock cannot expire a deadline, and then expires it all at once") {
    // Why the pumps could not simply be called from the iconified branch and left
    // on ImGui::GetTime(). App::pump_lineup_swap arms a 30 s give-up on the
    // dropped symbols' flatten; taking that branch stops keeping positions and
    // kill-switches instead. On a frame clock the deadline is unreachable while
    // hidden and then already blown on the first frame back — never anywhere in
    // between, so the flatten is never actually given its 30 seconds.
    FrameClock frame;
    frame.new_frame(1000.0);
    const double deadline = frame.now_s() + 30.0;

    for (int i = 0; i < 10'000; ++i)
        REQUIRE(frame.now_s() < deadline);   // frozen: the 30 s never elapses

    frame.new_frame(4600.0);                 // restored an hour later
    CHECK(frame.now_s() >= deadline);
    CHECK(frame.now_s() - (deadline - 30.0) == doctest::Approx(3600.0));

    // A steady clock crosses it when it is actually crossed.
    const MonoClock mono;
    const double armed = mono.now_s();
    CHECK(mono.now_s() < armed + 30.0);
}
