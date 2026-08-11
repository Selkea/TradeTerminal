#pragma once

// The host loop's tick cadence, and the clock every deadline in the app is on.
//
// THE 2026-08-11 INCIDENT. main.cpp used to skip App::draw() whenever the window
// was ICONIFIED, and draw() was where every pump_* ran. Minimizing the window
// therefore froze the 09:35 lineup build, the 09:25 scheduled auto-start, the
// 15:55 scheduled stop, all four watchdogs (broker-disconnect, orphaned
// position, history-staleness, pre-open gateway) and the autopilot's
// re-optimization — silently, because the gateway's own thread kept logging and
// /diag kept serving its last-built body with HTTP 200. The box is a Windows
// Server VPS driven over RDP, where a minimized window is an ordinary state.
// A dry run stalled about three minutes into a build and stayed stalled.
//
// TWO THINGS HAD TO CHANGE, and only both together are a fix:
//
//   1. The host loop must tick the app whether or not it renders it
//      (App::tick, called unconditionally from main.cpp).
//   2. The deadlines those pumps run on must be on a clock that advances
//      without a frame. ImGui::GetTime() does NOT: ImGui_ImplGlfw_NewFrame
//      accumulates io.DeltaTime from glfwGetTime, so with no NewFrame the clock
//      stands still — and then, on the first restored frame, jumps forward by
//      the ENTIRE minimized duration in one unclamped step. Calling the pumps
//      on that clock would have traded a silent freeze for every deadline in
//      the app expiring simultaneously on restore: the lineup swap's 30 s
//      flatten give-up (which kill-switches), the lineup fetch's 150 s
//      deadline, the tournament's 120 s abort, and a broker watchdog paging
//      with a down-time spanning the whole minimize.
//
// So: MonoClock is the one clock for anything with a real-world deadline, and
// ImGui::GetTime() is reserved for work that is genuinely per-frame (paced UI
// refreshes) and cannot outlive the frame loop.

#include <chrono>
#include <cstdint>

namespace tt::ui {

// How often the host loop calls App::tick() when it is NOT rendering.
//
// 10 ms, chosen against the tightest consumer rather than the loosest:
//
//  - It is exactly the interval the iconified path already slept on before this
//    fix (ImGui_ImplGlfw_Sleep(10) in main.cpp, i.e. ::Sleep on Windows), so the
//    not-rendering loop costs no more CPU than the code it replaces. It is a
//    paced sleep, not a spin — but that holds only as long as everything tick()
//    calls also blocks rather than spins while it waits. Beware
//    std::this_thread::sleep_for below 1 ms: it returns IMMEDIATELY on this
//    toolchain (mingw-w64 truncates nanosleep to whole ms), so it reads as
//    pacing and behaves as a spin. It made App::pump_sweep's result drain burn
//    74% of a core here at 10 ms, invisibly, because the same code had been
//    hidden inside a ~30 ms vsynced frame. Use wait_off_core() (block_wait.h).
//  - It is ~3x tighter than the ~33 ms the RENDERED loop achieves on the VPS
//    (glfwSwapInterval(1); measured ~30 ms wall against 5.84 ms of work, see
//    App::pump_sweep), so nothing that runs today gets slower when the window is
//    hidden. That matters most for the optimizer's result drain, which is bounded
//    per call by kSweepDrainBudgetMs (8 ms): 8 ms of draining inside a 10 ms
//    period is ~80% duty against ~24% for the same 8 ms inside a vsynced frame.
//  - Every other consumer is a timer with second resolution or coarser —
//    pump_diag rebuilds at 1 Hz, the watchdogs page at 60 s / 120 s, the
//    session schedule is minute-resolution wall clock — so 10 ms is two orders
//    of magnitude finer than any of them need.
//  - main.cpp already calls timeBeginPeriod(1), so a 10 ms sleep is a real
//    10 ms and not Windows' default ~15.6 ms granularity.
inline constexpr int kTickIntervalMs = 10;

// How long the host may sleep after a tick that took `spent_ms`.
//
// A PERIOD, not a delay. A tick that overran its slot (an 8 ms optimizer drain
// plus a /diag rebuild) goes straight into the next one instead of adding its
// cost on top of a fixed sleep — otherwise the busiest ticks, which are the ones
// with work waiting, would be the ones the loop paced hardest. It cannot become
// a spin: returning 0 requires the tick itself to have burned a full interval of
// wall clock, so the loop's duty cycle is bounded by the work, not by the sleep.
constexpr int tick_sleep_ms(int64_t spent_ms) {
    return spent_ms >= kTickIntervalMs ? 0
                                       : static_cast<int>(kTickIntervalMs - spent_ms);
}

// Steady seconds/milliseconds since an epoch. THE clock for every deadline that
// has to keep time on an iteration where no frame is built (see the header
// comment). Steady rather than wall, so an NTP correction — the VPS re-syncs
// overnight, around the same gateway restart the pre-open check is watching —
// cannot make a settle window look satisfied or a deadline look expired, which
// is what net/hist_freshness.h and net/bar_cache.h already refuse to trust the
// wall clock for.
class MonoClock {
public:
    double now_s() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_)
            .count();
    }
    int64_t now_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - epoch_)
            .count();
    }

private:
    std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();
};

} // namespace tt::ui
