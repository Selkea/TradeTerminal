#pragma once
// Built-in SMA-crossover strategy (long/flat). Proves the engine pipeline in
// Phase 2; ported to strategies/sma_crossover.cpp as a DLL in Phase 3.

#include "tt/strategy_api.h"

#include <cstdio>
#include <vector>

namespace tt {

class SmaCrossover final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        fast_ = static_cast<int>(ctx.param("fast", 10));
        slow_ = static_cast<int>(ctx.param("slow", 30));
        qty_ = ctx.param("qty", 100);
        if (fast_ < 1) fast_ = 1;
        if (slow_ <= fast_) slow_ = fast_ + 1;
        closes_.clear();
        closes_.reserve(1 << 20);  // no reallocation during the run
        sum_fast_ = sum_slow_ = 0.0;
        prev_diff_ = 0.0;
        prev_valid_ = false;
        sym_ = 0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SMA crossover: fast=%d slow=%d qty=%.0f",
                      fast_, slow_, qty_);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
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
        if (prev_valid_) {
            const double pos = ctx.position(sym_).qty;
            if (prev_diff_ <= 0.0 && diff > 0.0 && pos <= 0.0) {
                OrderRequest r{sym_, Side::Buy, OrdType::Market, {}, qty_, 0.0, 0.0,
                               0.0, 0.0};
                ctx.submit_order(r);
            } else if (prev_diff_ >= 0.0 && diff < 0.0 && pos > 0.0) {
                OrderRequest r{sym_, Side::Sell, OrdType::Market, {}, pos, 0.0, 0.0,
                               0.0, 0.0};
                ctx.submit_order(r);
            }
        }
        prev_diff_ = diff;
        prev_valid_ = true;
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "fill: %s %.0f @ %.2f (fee %.2f)",
                      f.side == Side::Buy ? "BUY" : "SELL", f.qty, f.price, f.fee);
        ctx.log(0, buf);
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "SMA crossover stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    int fast_ = 10, slow_ = 30;
    double qty_ = 100;
    std::vector<double> closes_;
    double sum_fast_ = 0, sum_slow_ = 0, prev_diff_ = 0;
    bool prev_valid_ = false;
    uint32_t sym_ = 0;
};

} // namespace tt
