// Bollinger Z-Score Reversion (long/flat) — band mean reversion.
//
// Entry:  z-score of close vs the `length`-bar mean drops to -`entry_z` or
//         below, optionally gated by a long-term trend filter (only buy dips
//         in an uptrend — mean reversion against the trend is how these
//         systems blow up).
// Exit:   z recovers to -`exit_z` (the reversion happened), or the position
//         ages past `time_stop` bars (the reversion didn't happen — take the
//         loss and move on). Deliberately NO price stop: hard stops on
//         mean-reversion entries systematically sell the lows; the time stop
//         is the risk control, and position size is the loss limiter.
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"length", 20, 5, 300},        // z-score lookback (bars)
    {"entry_z", 2.0, 0.5, 5},      // enter at or below -entry_z
    {"exit_z", 0.25, 0, 3},        // exit at or above -exit_z
    {"time_stop", 12, 0, 500},     // max bars in trade (0 = off)
    {"trend_len", 200, 0, 1000},   // SMA trend filter (0 = off)
    {"alloc_pct", 20, 1, 100},     // % of cash per entry
    {"max_qty", 5000, 1, 100000},  // hard share cap per position
};
}

class BollingerReversionStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        length_ = static_cast<int>(ctx.param("length", 20));
        entry_z_ = ctx.param("entry_z", 2.0);
        exit_z_ = ctx.param("exit_z", 0.25);
        time_stop_ = static_cast<int>(ctx.param("time_stop", 12));
        trend_len_ = static_cast<int>(ctx.param("trend_len", 200));
        alloc_pct_ = ctx.param("alloc_pct", 20);
        max_qty_ = ctx.param("max_qty", 5000);
        if (length_ < 2) length_ = 2;
        if (exit_z_ > entry_z_) exit_z_ = entry_z_;  // exit band inside entry band

        sym_ = 0;
        closes_.clear();
        closes_.reserve(1 << 20);
        sum_ = sum2_ = trend_sum_ = 0.0;
        entry_id_ = exit_id_ = 0;
        bars_held_ = 0;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "BollRev: len=%d entry=%.2fz exit=%.2fz tstop=%d trend=%d",
                      length_, entry_z_, exit_z_, time_stop_, trend_len_);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        closes_.push_back(bar.close);
        const size_t n = closes_.size();
        sum_ += bar.close;
        sum2_ += bar.close * bar.close;
        if (n > static_cast<size_t>(length_)) {
            const double old = closes_[n - 1 - length_];
            sum_ -= old;
            sum2_ -= old * old;
        }
        if (trend_len_ > 0) {
            trend_sum_ += bar.close;
            if (n > static_cast<size_t>(trend_len_))
                trend_sum_ -= closes_[n - 1 - trend_len_];
        }

        const size_t warm = static_cast<size_t>(std::max(length_, trend_len_));
        if (n < warm) return;

        const double mean = sum_ / length_;
        const double var = std::max(0.0, sum2_ / length_ - mean * mean);
        const double sd = std::sqrt(var);
        if (sd < 1e-12) return;  // dead tape: no meaningful z-score
        const double z = (bar.close - mean) / sd;

        const double pos = ctx.position(sym_).qty;
        if (pos > 0.0) {
            ++bars_held_;
            if (exit_id_ != 0) return;  // exit already in flight
            const bool reverted = z >= -exit_z_;
            const bool timed_out = time_stop_ > 0 && bars_held_ >= time_stop_;
            if (reverted || timed_out) {
                exit_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Market, {},
                                             pos, 0.0, 0.0, 0.0, 0.0});
                if (exit_id_)
                    ctx.log(0, reverted ? "exit: reverted to band" : "exit: time stop");
            }
            return;
        }

        bars_held_ = 0;
        if (entry_id_ != 0 || exit_id_ != 0) return;  // an order is in flight
        const bool trend_ok =
            trend_len_ == 0 || bar.close > trend_sum_ / trend_len_;
        if (z <= -entry_z_ && trend_ok) {
            const double cash = ctx.cash();
            double qty = std::floor(cash * (alloc_pct_ / 100.0) / bar.close);
            qty = std::min(qty, max_qty_);
            if (qty < 1.0) return;
            entry_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Market, {}, qty,
                                          0.0, 0.0, 0.0, 0.0});
            if (entry_id_) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "entry %.0f @ z=%.2f", qty, z);
                ctx.log(0, buf);
            }
        }
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext&, const Fill& f) noexcept override {
        if (f.order_id == entry_id_) {
            entry_id_ = 0;
            bars_held_ = 0;
        } else if (f.order_id == exit_id_) {
            exit_id_ = 0;
        }
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "BollRev stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    int length_ = 20, time_stop_ = 12, trend_len_ = 200;
    double entry_z_ = 2.0, exit_z_ = 0.25, alloc_pct_ = 20, max_qty_ = 5000;

    uint32_t sym_ = 0;
    std::vector<double> closes_;
    double sum_ = 0.0, sum2_ = 0.0, trend_sum_ = 0.0;
    uint64_t entry_id_ = 0, exit_id_ = 0;
    int bars_held_ = 0;
};

TT_STRATEGY(BollingerReversionStrategy, "Bollinger Reversion", kParams)
