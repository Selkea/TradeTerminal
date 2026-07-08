#pragma once
// Paper execution simulator. Orders wait modeled wire latency, then fill
// against observed prices with slippage + commission. Only ever sees
// now_ns() values — identical behavior in backtest replay and live paper.
//
// Brackets: a filled parent with take_profit/stop_loss spawns two exit
// children (limit TP + stop SL, one-cancels-other). Children get their own
// ids from the same sequence; their fills surface like any other fill, but
// no OrderRecord exists for them engine-side (they were never submitted).

#include "tt/events.h"

#include <cstdint>
#include <random>
#include <vector>

namespace tt {

// Commission model = IBKR Pro Fixed (US stocks): per-share rate with a $1
// minimum per order, capped at 1% of the trade's value.
struct ExecParams {
    int64_t latency_ns = 250'000;        // fixed one-way order latency
    int64_t latency_jitter_ns = 50'000;  // uniform [0, jitter)
    double slippage_bps = 1.0;           // market-order slippage, basis points
    double fee_per_share = 0.005;
    double min_fee = 1.0;
    double max_fee_pct = 1.0;            // fee cap, % of trade value (0 = off)
    uint64_t seed = 42;                  // deterministic latency jitter
};

class ExecSim {
public:
    explicit ExecSim(const ExecParams& p = {}) { reset(p); }

    void reset(const ExecParams& p) {
        params_ = p;
        rng_.seed(p.seed);
        pending_.clear();
        next_id_ = 1;
    }

    // Queues the order; it becomes fillable at now + latency.
    uint64_t submit(const OrderRequest& r, int64_t now_ns);
    bool cancel(uint64_t order_id);

    // Call on every observed price for a symbol (tick or bar close), after
    // the clock has advanced. Appends any resulting fills.
    void on_price(uint32_t symbol_id, double price, int64_t now_ns,
                  std::vector<Fill>& out);

    // Cancels everything; returns the cancelled ids (kill switch).
    std::vector<uint64_t> cancel_all();

    size_t open_orders() const { return pending_.size(); }

private:
    struct PendingOrder {
        uint64_t id;
        uint32_t symbol_id;
        Side side;
        OrdType type;
        double qty, limit_price, stop_price;
        double take_profit = 0, stop_loss = 0;   // bracket spec (parent only)
        uint64_t oco_sibling = 0;                // cancelled when this fills
        int64_t effective_ns;   // now + modeled latency at submit
    };

    double fee(double qty, double price) const;
    int64_t latency();

    ExecParams params_;
    std::mt19937_64 rng_;
    std::vector<PendingOrder> pending_;
    uint64_t next_id_ = 1;
};

} // namespace tt
