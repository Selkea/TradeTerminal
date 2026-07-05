#include "engine/exec_sim.h"

#include <algorithm>
#include <cmath>

namespace tt {

int64_t ExecSim::latency() {
    int64_t l = params_.latency_ns;
    if (params_.latency_jitter_ns > 0)
        l += static_cast<int64_t>(rng_() % static_cast<uint64_t>(params_.latency_jitter_ns));
    return l;
}

double ExecSim::fee(double qty) const {
    return std::max(params_.min_fee, std::abs(qty) * params_.fee_per_share);
}

uint64_t ExecSim::submit(const OrderRequest& r, int64_t now_ns) {
    if (r.qty <= 0.0) return 0;
    if (r.type == OrdType::Limit && r.limit_price <= 0.0) return 0;
    if (r.type == OrdType::Stop && r.stop_price <= 0.0) return 0;
    const uint64_t id = next_id_++;
    PendingOrder o{id, r.symbol_id, r.side, r.type,
                   r.qty, r.limit_price, r.stop_price,
                   r.take_profit, r.stop_loss, 0,
                   now_ns + latency()};
    pending_.push_back(o);
    return id;
}

std::vector<uint64_t> ExecSim::cancel_all() {
    std::vector<uint64_t> ids;
    ids.reserve(pending_.size());
    for (const PendingOrder& o : pending_) ids.push_back(o.id);
    pending_.clear();
    return ids;
}

bool ExecSim::cancel(uint64_t order_id) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->id == order_id) {
            const uint64_t sibling = it->oco_sibling;
            pending_.erase(it);
            // Cancelling one bracket leg cancels the group (like Alpaca).
            if (sibling)
                for (auto s = pending_.begin(); s != pending_.end(); ++s)
                    if (s->id == sibling) {
                        pending_.erase(s);
                        break;
                    }
            return true;
        }
    }
    return false;
}

void ExecSim::on_price(uint32_t symbol_id, double price, int64_t now_ns,
                       std::vector<Fill>& out) {
    std::vector<PendingOrder> filled;   // post-loop: OCO + bracket spawning
    for (auto it = pending_.begin(); it != pending_.end();) {
        const PendingOrder& o = *it;
        if (o.symbol_id != symbol_id || now_ns < o.effective_ns) {
            ++it;
            continue;
        }

        double fill_price = 0.0;
        bool fills = false;
        if (o.type == OrdType::Market) {
            const double slip = price * params_.slippage_bps / 10'000.0;
            fill_price = o.side == Side::Buy ? price + slip : price - slip;
            fills = true;
        } else if (o.type == OrdType::Limit) {
            if (o.side == Side::Buy && price <= o.limit_price) {
                fill_price = std::min(price, o.limit_price);
                fills = true;
            } else if (o.side == Side::Sell && price >= o.limit_price) {
                fill_price = std::max(price, o.limit_price);
                fills = true;
            }
        } else {  // Stop: triggers through the stop, then fills like a market
            if ((o.side == Side::Buy && price >= o.stop_price) ||
                (o.side == Side::Sell && price <= o.stop_price)) {
                const double slip = price * params_.slippage_bps / 10'000.0;
                fill_price = o.side == Side::Buy ? price + slip : price - slip;
                fills = true;
            }
        }

        if (fills) {
            out.push_back(Fill{o.id, o.symbol_id, o.side, {}, now_ns,
                               fill_price, o.qty, fee(o.qty)});
            filled.push_back(o);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    for (const PendingOrder& o : filled) {
        if (o.oco_sibling)   // one-cancels-other: drop the surviving leg
            for (auto s = pending_.begin(); s != pending_.end(); ++s)
                if (s->id == o.oco_sibling) {
                    pending_.erase(s);
                    break;
                }
        if (o.take_profit > 0.0 || o.stop_loss > 0.0) {
            const Side exit_side = o.side == Side::Buy ? Side::Sell : Side::Buy;
            const uint64_t tp_id = o.take_profit > 0.0 ? next_id_++ : 0;
            const uint64_t sl_id = o.stop_loss > 0.0 ? next_id_++ : 0;
            if (tp_id)
                pending_.push_back(PendingOrder{tp_id, o.symbol_id, exit_side,
                                                OrdType::Limit, o.qty, o.take_profit,
                                                0, 0, 0, sl_id, now_ns + latency()});
            if (sl_id)
                pending_.push_back(PendingOrder{sl_id, o.symbol_id, exit_side,
                                                OrdType::Stop, o.qty, 0, o.stop_loss,
                                                0, 0, tp_id, now_ns + latency()});
        }
    }
}

} // namespace tt
