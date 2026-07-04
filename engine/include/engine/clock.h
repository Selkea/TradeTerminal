#pragma once
// Time sources. The execution simulator and strategies only ever see
// now_ns(), so backtest replay and live paper trading run the identical
// code path — the clock is the seam.

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace tt {

inline uint64_t rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return 0;
#endif
}

// rdtsc <-> nanoseconds conversion, calibrated once at startup against
// QueryPerformanceCounter (~200 ms). Thread-safe after calibrate().
namespace tsc {
void calibrate();                 // idempotent
double ns_per_tick();             // 0.0 if not calibrated
int64_t to_ns(uint64_t ticks);
}

// Backtest time: set by the engine thread from each event's ts_event_ns.
class BacktestClock {
public:
    void set(int64_t ts_ns) { t_ = ts_ns; }
    int64_t now_ns() const { return t_; }

private:
    int64_t t_ = 0;
};

// Wall-clock epoch nanoseconds via rdtsc offset (cheap, monotonic-ish).
class RealTimeClock {
public:
    RealTimeClock();              // anchors rdtsc to system epoch time
    int64_t now_ns() const;

private:
    int64_t epoch_anchor_ns_;
    uint64_t tsc_anchor_;
};

} // namespace tt
