#pragma once
// Fixed log2-bucket latency histogram: zero allocation, a few ns per sample.
// Bucket i holds samples with bit_width(ns) == i, i.e. [2^(i-1), 2^i).

#include <array>
#include <bit>
#include <cstdint>

namespace tt {

class LatencyHistogram {
public:
    void record(int64_t ns) {
        if (ns < 0) ns = 0;
        const int b = std::bit_width(static_cast<uint64_t>(ns));
        buckets_[b < 63 ? b : 63]++;
        ++count_;
        if (ns > max_ns_) max_ns_ = ns;
    }

    uint64_t count() const { return count_; }
    int64_t max_ns() const { return max_ns_; }

    // Approximate percentile (bucket upper bound), p in [0,1].
    int64_t percentile_ns(double p) const {
        if (count_ == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_));
        uint64_t seen = 0;
        for (int i = 0; i < 64; ++i) {
            seen += buckets_[i];
            if (seen > target) return i == 0 ? 0 : (int64_t{1} << i) - 1;
        }
        return max_ns_;
    }

    void reset() {
        buckets_.fill(0);
        count_ = 0;
        max_ns_ = 0;
    }

    const std::array<uint64_t, 64>& buckets() const { return buckets_; }

private:
    std::array<uint64_t, 64> buckets_{};
    uint64_t count_ = 0;
    int64_t max_ns_ = 0;
};

} // namespace tt
