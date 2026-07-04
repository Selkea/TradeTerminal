#pragma once
// Single-producer / single-consumer lock-free ring buffer (Vyukov/rigtorp
// pattern). The hot-path primitive wiring feed -> engine -> UI.
//
// Contract: exactly ONE thread calls try_push, exactly ONE thread calls
// try_pop. Indices are free-running u64 (wrap after ~10^19 events). Each
// side keeps a cached copy of the other side's index so the steady-state
// fast path performs zero cross-core loads; the caches sit on their own
// cache lines to prevent false sharing.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tt {

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    // Producer thread only.
    bool try_push(const T& v) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ == N) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ == N) return false;  // full
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h == cached_tail_) return false;      // empty
        }
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate — exact only when called from the consumer thread.
    size_t size() const {
        return static_cast<size_t>(tail_.load(std::memory_order_acquire) -
                                   head_.load(std::memory_order_acquire));
    }

    static constexpr size_t capacity() { return N; }

private:
    alignas(64) std::atomic<uint64_t> head_{0};   // written by consumer
    alignas(64) std::atomic<uint64_t> tail_{0};   // written by producer
    alignas(64) uint64_t cached_head_ = 0;        // producer's stale view of head_
    alignas(64) uint64_t cached_tail_ = 0;        // consumer's stale view of tail_
    alignas(64) std::array<T, N> buf_;
};

} // namespace tt
