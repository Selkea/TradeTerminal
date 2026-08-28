// SMA crossover (long/flat) — the example TradeTerminal strategy.
//
// Edit this file, click Build in the Strategy Manager, and run a backtest —
// no terminal restart needed. See strategies/README.md for the SDK rules and
// for the patterns this file demonstrates: signals guarded by in-flight
// order ids (market orders fill on the NEXT price event, so a signal must
// not re-fire while its order is pending), and on_fill routed by order id.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"fast", 10, 1, 500},     // fast SMA period (bars)
    {"slow", 30, 2, 1000},    // slow SMA period (bars)
    {"qty", 100, 1, 100000},  // shares per entry
};
}

class SmaCrossoverStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        fast_ = static_cast<int>(ctx.param("fast", 10));
        slow_ = static_cast<int>(ctx.param("slow", 30));
        qty_ = ctx.param("qty", 100);
        if (fast_ < 1) fast_ = 1;
        // REFUSE, DO NOT REPAIR. This line used to read
        //
        //     if (slow_ <= fast_) slow_ = fast_ + 1;
        //
        // and that repair MANUFACTURED the pathology it looks like it prevents.
        // The optimizer sweeps one parameter at a time, holding the others at
        // the incumbent (App::start_sweep_cell), so while it walks `fast` across
        // its declared [1,500] with `slow` frozen at the default 30, every grid
        // point above 30 silently became (fast, fast+1). Eleven of the twelve
        // cells on that pass ran as adjacent moving averages, were scored as if
        // they were distinct, and one of them got crowned: MRNA went live on
        // 2026-08-27 with fast=273 slow=274 and lost $110 in a single round trip
        // (in 142.73, out 139.58, back in 139.80 ten minutes later).
        //
        // Adjacent SMAs are not a slow signal, they are a noise generator. With
        // slow = fast+1 the difference of the two sums is a single close, so
        // `diff` reduces to (SMA_fast - close[n-slow]) / slow: a smooth mean
        // measured against one old bar, which crosses zero on tick noise no
        // matter how long the lookback. That is why nothing downstream caught
        // it — the 0.35.0 minimum-trade floor is one-sided, and a whipsaw fails
        // no test that only asks for ENOUGH trades.
        //
        // Refusing costs nothing and is scored: a disabled strategy takes zero
        // trades, lands under sweep_min_trades, and the sweep marks the whole
        // infeasible region rejected instead of ranking eleven copies of the
        // same physics. Same doctrine as symbol_params.h's unreachable time
        // stop: "it would leave the symbol trading a number no backtest ever
        // scored." A quarter's separation, floor of two bars, so a legitimately
        // tight pair like (10, 13) still trades.
        const int min_slow = std::max(fast_ + 2, static_cast<int>(fast_ * 1.25));
        disabled_ = slow_ < min_slow;
        closes_.clear();
        closes_.reserve(1 << 20);
        sum_fast_ = sum_slow_ = prev_diff_ = 0.0;
        prev_valid_ = false;
        sym_ = 0;
        entry_id_ = exit_id_ = 0;
        char buf[160];
        if (disabled_)
            std::snprintf(buf, sizeof(buf),
                          "SMA(dll): DISABLED - slow=%d is not clear of fast=%d "
                          "(needs >= %d); adjacent averages cross on noise",
                          slow_, fast_, min_slow);
        else
            std::snprintf(buf, sizeof(buf), "SMA(dll): fast=%d slow=%d qty=%.0f",
                          fast_, slow_, qty_);
        // Level 2, not 3: level 3 renders as [strategy error] and pages
        // Critical (alert_rules.h). A set that simply is not trading is
        // worth saying loudly in the log and not worth waking anyone.
        ctx.log(disabled_ ? 2 : 1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        // Refused in on_init; place nothing, ever.
        //
        // Safe to return before the exit branch below because a disabled
        // instance can never be holding its own position: on_init runs either at
        // session start (before anything trades) or on a hot swap, and since
        // 0.39.0 a swap requires the symbol to be flat AND have nothing working.
        // An ADOPTED position is not this strategy's to close — its strategy is
        // paused and the broker-side stop exits it.
        if (disabled_) return;
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        closes_.push_back(bar.close);
        const size_t n = closes_.size();
        sum_fast_ += bar.close;
        if (n > static_cast<size_t>(fast_)) sum_fast_ -= closes_[n - 1 - fast_];
        sum_slow_ += bar.close;
        if (n > static_cast<size_t>(slow_)) sum_slow_ -= closes_[n - 1 - slow_];
        if (n < static_cast<size_t>(slow_)) return;

        const double diff = sum_fast_ / fast_ - sum_slow_ / slow_;
        if (prev_valid_ && entry_id_ == 0 && exit_id_ == 0) {
            const double pos = ctx.position(sym_).qty;
            if (prev_diff_ <= 0.0 && diff > 0.0 && pos <= 0.0) {
                // Never buy more than the risk budget carries (the engine caps
                // the position there regardless).
                const double qty =
                    std::min(qty_, std::floor(ctx.budget(sym_) / bar.close));
                if (qty >= 1.0)
                    entry_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Market,
                                                  {}, qty, 0.0, 0.0, 0.0, 0.0});
            } else if (prev_diff_ >= 0.0 && diff < 0.0 && pos > 0.0) {
                exit_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Market,
                                             {}, pos, 0.0, 0.0, 0.0, 0.0});
            }
        }
        prev_diff_ = diff;
        prev_valid_ = true;
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        if (f.order_id == entry_id_) entry_id_ = 0;
        else if (f.order_id == exit_id_) exit_id_ = 0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "fill: %s %.0f @ %.2f",
                      f.side == Side::Buy ? "BUY" : "SELL", f.qty, f.price);
        ctx.log(0, buf);
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "SMA(dll) stopped");
    }

    void destroy() noexcept override { delete this; }

    // Without this, one rejected order leaves entry_id_/exit_id_ set and the
    // `entry_id_ == 0 && exit_id_ == 0` guard blocks the rest of the session.
    void on_order_end(IStrategyContext&, const OrderEnd& e) noexcept override {
        if (e.order_id == entry_id_) entry_id_ = 0;
        else if (e.order_id == exit_id_) exit_id_ = 0;
    }

private:
    int fast_ = 10, slow_ = 30;
    // Set in on_init when slow is not clear enough of fast. A disabled
    // instance places nothing, so the optimizer scores the region as
    // zero-trade and rejects it instead of crowning a whipsaw.
    bool disabled_ = false;
    double qty_ = 100;
    std::vector<double> closes_;
    double sum_fast_ = 0, sum_slow_ = 0, prev_diff_ = 0;
    bool prev_valid_ = false;
    uint32_t sym_ = 0;
    uint64_t entry_id_ = 0, exit_id_ = 0;
};

TT_STRATEGY(SmaCrossoverStrategy, "SMA Crossover", kParams)
