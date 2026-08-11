#pragma once

// A wait that actually takes the calling thread OFF the core.
//
// WHY THIS EXISTS. std::this_thread::sleep_for DOES NOT SLEEP for
// sub-millisecond durations on this toolchain (MSYS2 UCRT64 GCC): mingw-w64's
// nanosleep truncates its argument to whole milliseconds, so sleep_for(250us)
// returns immediately and any loop "paced" by it is a spin. It reads as pacing
// at the call site and behaves as a spin at runtime, which is the worst possible
// combination — measured here at both -O0 and -O2:
//
//     sleep_for(250us)   27-63 ns per call    ~97% of a core
//     sleep_for(500us)   27-61 ns per call    ~98% of a core
//     sleep_for(1ms)     15.6 ms per call     ~0%  of a core   (see below)
//     ::Sleep(1)         ~1.9 ms per call     ~0%  of a core
//
// THE INCIDENT IT CAME OUT OF (2026-08-11). App::pump_sweep's optimizer result
// drain waits on a backtest already in flight on the engine thread, bounded by
// kSweepDrainBudgetMs (8 ms), and did that waiting on sleep_for(250us). While
// pump_sweep only ran from draw(), the spin was hidden inside a ~30 ms vsynced
// frame and cost ~26% of a core. 0.19.0 moved every pump onto App::tick() so a
// minimized window would stop freezing the schedule — and tick() runs every
// 10 ms, which turned the same code into 74% of a core (measured on a replica of
// the drain loop), taken from the engine thread running those very backtests and
// from the live session's thread beside it, on a VPS with few cores to spare.
// tick_clock.h promises the not-rendering loop costs no more CPU than the code
// it replaced; this is the primitive that keeps that promise true.
//
// std::this_thread::sleep_for(1ms) IS NOT A SUBSTITUTE, despite blocking. It
// ignores main.cpp's timeBeginPeriod(1) — mingw's nanosleep waits on a plain
// waitable timer, which stays at Windows' default ~15.6 ms granularity — so a
// single wait would overrun the whole 8 ms drain budget. ::Sleep does honour
// timeBeginPeriod, hence the split below rather than a portable one-liner.

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <thread>
#endif

namespace tt::ui {

// Block for about a millisecond. The contract callers depend on is "this yields
// the core", not the exact duration: granularity is ~1.9 ms in practice with
// timeBeginPeriod(1) in force, which is why callers bound their waiting with a
// budget rather than counting iterations.
inline void wait_off_core() {
#ifdef _WIN32
    ::Sleep(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

} // namespace tt::ui
