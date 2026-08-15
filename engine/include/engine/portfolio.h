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

    // Broker reconciliation at session start: adopt an existing position /
    // the real account cash without booking a fill.
    void seed_position(uint32_t symbol_id, double qty, double avg_price) {
        Slot& s = slot(symbol_id);
        s.qty = qty;
        s.avg_price = avg_price;
        if (s.last_price == 0.0) s.last_price = avg_price;
    }
    void set_cash(double cash) { cash_ = cash; }

    double cash() const { return cash_; }
    double equity() const;
    Position position(uint32_t symbol_id) const;

    // COMMISSIONS PAID on this symbol (or across the book). Position::realized
    // is gross of them — apply() books (price - avg) * qty and charges the fee
    // only to cash_ — so every realized-P&L surface in the product understates
    // a loss by exactly this much.
    //
    // 2026-08-14 closed -$597.01 realized against -$612.09 of actual cash: 13
    // fills, $15.08 of commission, 2.5% of the number used to judge whether the
    // strategies have an edge. It scales with trade count, and a scalper's
    // does not stay at 2.5%.
    //
    // `realized` is deliberately LEFT gross rather than quietly redefined:
    // journal rows, saved sessions and every historical comparison were computed
    // that way, and silently shifting the meaning of a number under them is how
    // a P&L series stops being comparable to itself. Both are published; net is
    // the one to judge on.
    double fees(uint32_t symbol_id) const;
    double fees() const;
    double realized_net(uint32_t symbol_id) const;

    int wins() const { return wins_; }
    int losses() const { return losses_; }

private:
    struct Slot {
        uint32_t symbol_id = 0;
        double qty = 0.0;        // signed
        double avg_price = 0.0;
        double realized = 0.0;   // GROSS of commissions (see fees())
        double fees = 0.0;       // commissions charged to this symbol
        double last_price = 0.0;
    };
    Slot& slot(uint32_t symbol_id);
    const Slot* find(uint32_t symbol_id) const;

    double cash_;
    std::vector<Slot> slots_;   // few symbols: linear scan beats hashing
    int wins_ = 0, losses_ = 0;
};

} // namespace tt
