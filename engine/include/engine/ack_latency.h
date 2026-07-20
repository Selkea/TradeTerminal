#pragma once
// Order-path latency measured live and fed into the fill simulator.
//
// A broker's I/O thread calls record_ms() on every submit->ack round-trip; the
// UI thread calls summary() to persist a median + spread that the backtest and
// optimizer use for ExecParams, so their fills model the real venue path
// instead of the 250 us default.
//
// Linear 1 ms buckets give the median ms resolution. A multi-second hung ack
// (e.g. the 27 s half-open outliers seen live) lands in the overflow bin: it
// bumps the count but not the median, so p50/p90 stay in the real range unless
// a genuine majority of orders are that slow. Acks arrive at human order-rate,
// so the lock is never contended.

#include <array>
#include <cstdint>
#include <mutex>

namespace tt {

// Base + jitter (ns) for ExecParams, derived from the ack distribution.
// base = median, jitter spread = p90 - p50, so the simulator's uniform
// [base, base+jitter) roughly spans the p50..p90 of real acks. count == 0
// means nothing was measured and the caller should keep its default.
struct AckSummary {
    uint64_t count = 0;
    int64_t base_ns = 0;
    int64_t jitter_ns = 0;
};

class AckLatency {
public:
    void record_ms(int64_t ms) {
        if (ms < 0) ms = 0;
        const size_t u = static_cast<size_t>(ms);
        std::lock_guard<std::mutex> g(m_);
        buckets_[u < kMaxMs ? u : kMaxMs]++;
        ++count_;
    }

    AckSummary summary() const {
        std::lock_guard<std::mutex> g(m_);
        if (count_ == 0) return {};
        const int64_t p50 = pct(0.50);
        const int64_t p90 = pct(0.90);
        return {count_, p50 * 1'000'000, (p90 > p50 ? p90 - p50 : 0) * 1'000'000};
    }

private:
    // Milliseconds at percentile p; caller holds m_.
    int64_t pct(double p) const {
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_));
        uint64_t seen = 0;
        for (size_t i = 0; i <= kMaxMs; ++i) {
            seen += buckets_[i];
            if (seen > target) return static_cast<int64_t>(i);
        }
        return kMaxMs;
    }

    static constexpr size_t kMaxMs = 2000;   // buckets_[kMaxMs] = overflow bin
    std::array<uint32_t, kMaxMs + 1> buckets_{};
    uint64_t count_ = 0;
    mutable std::mutex m_;
};

} // namespace tt
