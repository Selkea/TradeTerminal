// RSI-2 Pullback (long/flat) — the Connors short-term pullback system.
//
// Buy weakness inside an uptrend, sell the first strength:
//   - regime filter: only long while close > SMA(`trend_ma`)
//   - entry: Wilder RSI(`rsi_len`) at or below `buy_below`
//   - exit: close crosses above SMA(`exit_ma`)
// No price stop, by design: the edge is buying short-term panic, and stops
// placed inside the panic get hit systematically. The regime filter plus the
// small fixed allocation are the risk controls (Connors' published results
// degrade with stops attached — keep size modest instead).
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"rsi_len", 2, 2, 14},          // RSI period (Wilder)
    {"buy_below", 10, 1, 40},       // entry threshold
    {"exit_ma", 5, 2, 50},          // exit SMA (bars)
    {"trend_ma", 200, 20, 1000},    // regime SMA (bars)
    {"alloc_pct", 25, 1, 100},      // % of cash per entry
    {"max_qty", 5000, 1, 100000},   // hard share cap per position
};
}

class Rsi2PullbackStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        rsi_len_ = static_cast<int>(ctx.param("rsi_len", 2));
        buy_below_ = ctx.param("buy_below", 10);
        exit_ma_ = static_cast<int>(ctx.param("exit_ma", 5));
        trend_ma_ = static_cast<int>(ctx.param("trend_ma", 200));
        alloc_pct_ = ctx.param("alloc_pct", 25);
        max_qty_ = ctx.param("max_qty", 5000);
        if (rsi_len_ < 2) rsi_len_ = 2;
        if (exit_ma_ < 2) exit_ma_ = 2;
        if (trend_ma_ < exit_ma_) trend_ma_ = exit_ma_;

        sym_ = 0;
        closes_.clear();
        closes_.reserve(1 << 20);
        exit_sum_ = trend_sum_ = 0.0;
        prev_close_ = 0.0;
        avg_gain_ = avg_loss_ = 0.0;
        rsi_n_ = 0;
        entry_id_ = exit_id_ = 0;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "RSI2: len=%d buy<%.0f exitMA=%d trendMA=%d alloc=%.0f%%",
                      rsi_len_, buy_below_, exit_ma_, trend_ma_, alloc_pct_);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        // Wilder RSI: seed with simple averages, then smooth.
        if (prev_close_ > 0.0) {
            const double chg = bar.close - prev_close_;
            const double gain = chg > 0.0 ? chg : 0.0;
            const double loss = chg < 0.0 ? -chg : 0.0;
            if (rsi_n_ < rsi_len_) {
                avg_gain_ += gain;
                avg_loss_ += loss;
                if (++rsi_n_ == rsi_len_) {
                    avg_gain_ /= rsi_len_;
                    avg_loss_ /= rsi_len_;
                }
            } else {
                avg_gain_ = (avg_gain_ * (rsi_len_ - 1) + gain) / rsi_len_;
                avg_loss_ = (avg_loss_ * (rsi_len_ - 1) + loss) / rsi_len_;
            }
        }
        prev_close_ = bar.close;

        closes_.push_back(bar.close);
        const size_t n = closes_.size();
        exit_sum_ += bar.close;
        if (n > static_cast<size_t>(exit_ma_)) exit_sum_ -= closes_[n - 1 - exit_ma_];
        trend_sum_ += bar.close;
        if (n > static_cast<size_t>(trend_ma_)) trend_sum_ -= closes_[n - 1 - trend_ma_];

        if (n < static_cast<size_t>(trend_ma_) || rsi_n_ < rsi_len_) return;

        const double denom = avg_gain_ + avg_loss_;
        const double rsi = denom > 1e-12 ? 100.0 * avg_gain_ / denom : 50.0;

        const double pos = ctx.position(sym_).qty;
        if (pos > 0.0) {
            if (exit_id_ == 0 && bar.close > exit_sum_ / exit_ma_) {
                exit_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Market, {},
                                             pos, 0.0, 0.0, 0.0, 0.0});
                if (exit_id_) ctx.log(0, "exit: close above exit MA");
            }
            return;
        }

        if (entry_id_ != 0 || exit_id_ != 0) return;  // an order is in flight
        if (bar.close > trend_sum_ / trend_ma_ && rsi <= buy_below_) {
            const double cash = ctx.cash();
            double qty = std::floor(cash * (alloc_pct_ / 100.0) / bar.close);
            qty = std::min(qty, max_qty_);
            if (qty < 1.0) return;
            entry_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Market, {}, qty,
                                          0.0, 0.0, 0.0, 0.0});
            if (entry_id_) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "entry %.0f @ RSI %.1f", qty, rsi);
                ctx.log(0, buf);
            }
        }
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext&, const Fill& f) noexcept override {
        if (f.order_id == entry_id_) entry_id_ = 0;
        else if (f.order_id == exit_id_) exit_id_ = 0;
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "RSI2 stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    int rsi_len_ = 2, exit_ma_ = 5, trend_ma_ = 200;
    double buy_below_ = 10, alloc_pct_ = 25, max_qty_ = 5000;

    uint32_t sym_ = 0;
    std::vector<double> closes_;
    double exit_sum_ = 0.0, trend_sum_ = 0.0;
    double prev_close_ = 0.0, avg_gain_ = 0.0, avg_loss_ = 0.0;
    int rsi_n_ = 0;
    uint64_t entry_id_ = 0, exit_id_ = 0;
};

TT_STRATEGY(Rsi2PullbackStrategy, "RSI-2 Pullback", kParams)
