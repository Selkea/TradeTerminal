#pragma once
// Snap an order price to a venue-legal price increment.
//
// IBKR rejects any limit/stop price that is not a multiple of the contract's
// minimum price variation with error 110 ("The price does not conform to the
// minimum price variation for this contract"). Strategy exit math produces raw
// floating-point products — e.g. f.price * (1 + tp_bps/10000) = 205.870585 —
// which are sub-penny and get rejected. This is a broker/exchange constraint
// (what the real venue accepts), so brokers snap here; the fill simulator does
// not, keeping backtests/replays bit-identical.
//
// SEAM: the increment is the US-equity default (SEC Reg NMS Rule 612: $0.01 at
// or above $1.00, $0.0001 below). Per-contract minTick from reqContractDetails
// can replace tick_size_for() once that is plumbed through the adapters.

#include <cmath>

namespace tt {

inline double tick_size_for(double price) noexcept {
    return price < 1.0 ? 0.0001 : 0.01;
}

// Round price to the nearest legal increment. Non-positive / unset / NaN prices
// (a market order carries 0.0 here) pass through untouched.
inline double snap_to_tick(double price) noexcept {
    if (!(price > 0.0)) return price;
    const double tick = tick_size_for(price);
    return std::round(price / tick) * tick;
}

} // namespace tt
