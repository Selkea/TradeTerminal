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
    const uint64_t id = next_id_++;
    pending_.push_back(PendingOrder{id, r.symbol_id, r.side, r.type,
                                    r.qty, r.limit_price, now_ns + latency()});
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
            pending_.erase(it);
            return true;
        }
    }
    return false;
}

void ExecSim::on_price(uint32_t symbol_id, double price, int64_t now_ns,
                       std::vector<Fill>& out) {
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
        } else {  // Limit
            if (o.side == Side::Buy && price <= o.limit_price) {
                fill_price = std::min(price, o.limit_price);
                fills = true;
            } else if (o.side == Side::Sell && price >= o.limit_price) {
                fill_price = std::max(price, o.limit_price);
                fills = true;
            }
        }

        if (fills) {
            out.push_back(Fill{o.id, o.symbol_id, o.side, {}, now_ns,
                               fill_price, o.qty, fee(o.qty)});
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace tt
