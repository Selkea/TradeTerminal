#include "engine/portfolio.h"

#include <algorithm>
#include <cmath>

namespace tt {

Portfolio::Slot& Portfolio::slot(uint32_t symbol_id) {
    for (Slot& s : slots_)
        if (s.symbol_id == symbol_id) return s;
    slots_.push_back(Slot{symbol_id});
    return slots_.back();
}

const Portfolio::Slot* Portfolio::find(uint32_t symbol_id) const {
    for (const Slot& s : slots_)
        if (s.symbol_id == symbol_id) return &s;
    return nullptr;
}

void Portfolio::apply(const Fill& f) {
    Slot& s = slot(f.symbol_id);
    const double d = (f.side == Side::Buy) ? f.qty : -f.qty;

    cash_ -= d * f.price;   // buy consumes cash, sell frees it
    cash_ -= f.fee;

    const bool same_direction = s.qty == 0.0 || (s.qty > 0) == (d > 0);
    if (same_direction) {
        const double new_qty = s.qty + d;
        s.avg_price = new_qty != 0.0
                          ? (s.avg_price * s.qty + f.price * d) / new_qty
                          : 0.0;
        s.qty = new_qty;
    } else {
        // Reducing (possibly crossing through zero): book realized PnL on the
        // closed portion.
        const double closed = std::min(std::abs(d), std::abs(s.qty));
        const double direction = s.qty > 0 ? 1.0 : -1.0;
        const double pnl = (f.price - s.avg_price) * closed * direction;
        s.realized += pnl;
        (pnl >= 0 ? wins_ : losses_)++;

        const double remaining = s.qty + d;
        if ((remaining > 0) != (s.qty > 0) && remaining != 0.0) {
            s.avg_price = f.price;   // flipped: leftover opens at fill price
        } else if (remaining == 0.0) {
            s.avg_price = 0.0;
        }
        s.qty = remaining;
    }
    s.last_price = f.price;
}

void Portfolio::mark(uint32_t symbol_id, double price) {
    for (Slot& s : slots_)
        if (s.symbol_id == symbol_id) {
            s.last_price = price;
            return;
        }
}

double Portfolio::equity() const {
    double eq = cash_;
    for (const Slot& s : slots_) eq += s.qty * s.last_price;
    return eq;
}

Position Portfolio::position(uint32_t symbol_id) const {
    const Slot* s = find(symbol_id);
    if (!s) return Position{symbol_id, 0, 0, 0, 0};
    return Position{symbol_id, s->qty, s->avg_price,
                    (s->last_price - s->avg_price) * s->qty, s->realized};
}

} // namespace tt
