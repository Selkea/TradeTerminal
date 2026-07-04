#pragma once
// Positions, cash, PnL. Engine-thread only — no locks. Signed quantities
// (negative = short); average-price accounting with realized PnL booked on
// position-reducing fills.

#include "tt/events.h"

#include <cstdint>
#include <vector>

namespace tt {

class Portfolio {
public:
    explicit Portfolio(double initial_cash = 0.0) : cash_(initial_cash) {}

    void reset(double initial_cash) {
        cash_ = initial_cash;
        slots_.clear();
        wins_ = losses_ = 0;
    }

    void apply(const Fill& f);
    void mark(uint32_t symbol_id, double price);  // update last trade price

    double cash() const { return cash_; }
    double equity() const;
    Position position(uint32_t symbol_id) const;

    int wins() const { return wins_; }
    int losses() const { return losses_; }

private:
    struct Slot {
        uint32_t symbol_id = 0;
        double qty = 0.0;        // signed
        double avg_price = 0.0;
        double realized = 0.0;
        double last_price = 0.0;
    };
    Slot& slot(uint32_t symbol_id);
    const Slot* find(uint32_t symbol_id) const;

    double cash_;
    std::vector<Slot> slots_;   // few symbols: linear scan beats hashing
    int wins_ = 0, losses_ = 0;
};

} // namespace tt
