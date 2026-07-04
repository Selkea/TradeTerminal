#include "engine/clock.h"

#include <windows.h>

#include <atomic>
#include <chrono>

namespace tt::tsc {

namespace {
std::atomic<double> g_ns_per_tick{0.0};
}

void calibrate() {
    if (g_ns_per_tick.load(std::memory_order_acquire) > 0.0) return;

    LARGE_INTEGER freq, q0, q1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&q0);
    const uint64_t t0 = rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&q1);
    const uint64_t t1 = rdtsc();

    const double elapsed_ns =
        static_cast<double>(q1.QuadPart - q0.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart);
    const double ticks = static_cast<double>(t1 - t0);
    if (ticks > 0.0)
        g_ns_per_tick.store(elapsed_ns / ticks, std::memory_order_release);
}

double ns_per_tick() { return g_ns_per_tick.load(std::memory_order_acquire); }

int64_t to_ns(uint64_t ticks) {
    return static_cast<int64_t>(static_cast<double>(ticks) * ns_per_tick());
}

} // namespace tt::tsc

namespace tt {

RealTimeClock::RealTimeClock() {
    tsc::calibrate();
    epoch_anchor_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    tsc_anchor_ = rdtsc();
}

int64_t RealTimeClock::now_ns() const {
    return epoch_anchor_ns_ + tsc::to_ns(rdtsc() - tsc_anchor_);
}

} // namespace tt
