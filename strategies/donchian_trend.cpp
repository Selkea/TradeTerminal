// Donchian ATR Trend (long/flat) — Turtle-style trend following.
//
// Entry:  close breaks above the prior `entry_len`-bar high.
// Sizing: risk `risk_pct`% of cash per trade; share count = risk / (stop_atr
//         ATRs), so position size shrinks as volatility rises.
// Stop:   resting Stop order `stop_atr` ATRs below entry, trailed up
//         chandelier-style from the highest close since entry. The stop is a
//         real resting order (fills intrabar on synth ticks), not a
//         close-based check.
// Exit:   close breaks below the prior `exit_len`-bar low (classic Turtle
//         channel exit), or the trailed stop fires.
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <deque>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"entry_len", 20, 5, 400},     // breakout channel (bars)
    {"exit_len", 10, 2, 400},      // exit channel (bars)
    {"atr_len", 14, 2, 200},       // ATR period (Wilder)
    {"risk_pct", 0.5, 0.05, 5},    // % of cash risked per trade
    {"stop_atr", 2.0, 0.5, 10},    // stop distance in ATRs
    {"max_qty", 5000, 1, 100000},  // hard share cap per position
    // Entry window, local hours (9.5 = 09:30). 0/24 = always; exits not gated.
    {"enter_from_h", 0, 0, 24},
    {"enter_until_h", 24, 0, 24},
};

// Rolling extreme over the trailing N bars, O(1) amortized.
struct MonoDeque {
    std::deque<std::pair<int64_t, double>> d;
    // keep_max=true tracks the window max, false the min.
    void push(int64_t i, double v, bool keep_max) {
        while (!d.empty() &&
               (keep_max ? d.back().second <= v : d.back().second >= v))
            d.pop_back();
        d.emplace_back(i, v);
    }
    void evict_before(int64_t min_index) {
        while (!d.empty() && d.front().first < min_index) d.pop_front();
    }
    bool empty() const { return d.empty(); }
    double front() const { return d.front().second; }
    void clear() { d.clear(); }
};
} // namespace

class DonchianTrendStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        entry_len_ = static_cast<int>(ctx.param("entry_len", 20));
        exit_len_ = static_cast<int>(ctx.param("exit_len", 10));
        atr_len_ = static_cast<int>(ctx.param("atr_len", 14));
        risk_pct_ = ctx.param("risk_pct", 0.5);
        stop_atr_ = ctx.param("stop_atr", 2.0);
        max_qty_ = ctx.param("max_qty", 5000);
        enter_from_h_ = ctx.param("enter_from_h", 0);
        enter_until_h_ = ctx.param("enter_until_h", 24);
        if (entry_len_ < 2) entry_len_ = 2;
        if (exit_len_ < 2) exit_len_ = 2;
        if (atr_len_ < 2) atr_len_ = 2;
        if (stop_atr_ < 0.1) stop_atr_ = 0.1;

        sym_ = 0;
        bar_i_ = 0;
        highs_.clear();
        lows_.clear();
        prev_close_ = 0.0;
        atr_ = 0.0;
        tr_sum_ = 0.0;
        tr_n_ = 0;
        entry_id_ = stop_id_ = exit_id_ = 0;
        stop_px_ = 0.0;
        highest_close_ = 0.0;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Donchian: entry=%d exit=%d atr=%d risk=%.2f%% stop=%.1fATR",
                      entry_len_, exit_len_, atr_len_, risk_pct_, stop_atr_);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        // ATR (Wilder). True range needs the previous close.
        if (prev_close_ > 0.0) {
            const double tr =
                std::max(bar.high - bar.low,
                         std::max(std::abs(bar.high - prev_close_),
                                  std::abs(bar.low - prev_close_)));
            if (tr_n_ < atr_len_) {
                tr_sum_ += tr;
                if (++tr_n_ == atr_len_) atr_ = tr_sum_ / atr_len_;
            } else {
                atr_ = (atr_ * (atr_len_ - 1) + tr) / atr_len_;
            }
        }
        prev_close_ = bar.close;

        // Signals compare against the channel of the bars BEFORE this one.
        const bool warmed = bar_i_ >= entry_len_ && atr_ > 0.0;
        const double pos = ctx.position(sym_).qty;

        if (pos > 0.0) {
            manage_long(ctx, bar, pos);
        } else if (warmed && entry_id_ == 0 && exit_id_ == 0 && !highs_.empty() &&
                   bar.close > highs_.front()) {
            // Time-of-day gate on new entries only; exits are never gated.
            const double hod = hour_of_day_local(bar.ts_ns);
            if (hod >= enter_from_h_ && hod < enter_until_h_) open_long(ctx, bar.close);
        }

        // Roll the channels: current bar enters the window, old bars leave.
        highs_.push(bar_i_, bar.high, /*keep_max=*/true);
        lows_.push(bar_i_, bar.low, /*keep_max=*/false);
        ++bar_i_;
        highs_.evict_before(bar_i_ - entry_len_);
        lows_.evict_before(bar_i_ - exit_len_);
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        char buf[128];
        if (f.order_id == entry_id_) {
            entry_id_ = 0;
            highest_close_ = f.price;
            place_stop(ctx, f.qty, f.price - stop_atr_ * atr_);
            std::snprintf(buf, sizeof(buf), "entry %.0f @ %.2f, stop %.2f",
                          f.qty, f.price, stop_px_);
            ctx.log(1, buf);
        } else if (f.order_id == stop_id_) {
            stop_id_ = 0;
            stop_px_ = 0.0;
            std::snprintf(buf, sizeof(buf), "stopped out %.0f @ %.2f", f.qty, f.price);
            ctx.log(1, buf);
        } else if (f.order_id == exit_id_) {
            exit_id_ = 0;
            std::snprintf(buf, sizeof(buf), "channel exit %.0f @ %.2f", f.qty, f.price);
            ctx.log(1, buf);
        }
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "Donchian stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    void open_long(IStrategyContext& ctx, double price) noexcept {
        const double cash = ctx.cash();
        const double per_share_risk = stop_atr_ * atr_;
        if (per_share_risk <= 0.0 || price <= 0.0) return;
        double qty = std::floor(cash * (risk_pct_ / 100.0) / per_share_risk);
        qty = std::min(qty, std::floor(cash * 0.95 / price));  // never over-spend
        qty = std::min(qty, max_qty_);
        if (qty < 1.0) return;
        entry_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Market, {}, qty, 0.0,
                                      0.0, 0.0, 0.0});
        if (entry_id_ == 0) ctx.log(2, "entry rejected");
    }

    void place_stop(IStrategyContext& ctx, double qty, double px) noexcept {
        if (px <= 0.0 || qty <= 0.0) return;
        stop_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Stop, {}, qty, 0.0,
                                     px, 0.0, 0.0});
        stop_px_ = stop_id_ ? px : 0.0;
        if (stop_id_ == 0) ctx.log(3, "protective stop rejected — position unprotected");
    }

    void manage_long(IStrategyContext& ctx, const Bar& bar, double pos) noexcept {
        highest_close_ = std::max(highest_close_, bar.close);

        // Channel exit beats the trail: flat out at market, drop the stop.
        if (exit_id_ == 0 && !lows_.empty() && bar_i_ >= exit_len_ &&
            bar.close < lows_.front()) {
            if (stop_id_ && ctx.cancel_order(stop_id_)) {
                stop_id_ = 0;
                stop_px_ = 0.0;
            }
            exit_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Market, {}, pos,
                                         0.0, 0.0, 0.0, 0.0});
            return;
        }

        // Unprotected (stop rejected earlier): retry before trailing.
        if (stop_id_ == 0 && exit_id_ == 0) {
            place_stop(ctx, pos, highest_close_ - stop_atr_ * atr_);
            return;
        }

        // Chandelier trail: only ratchets up, and only by a meaningful step —
        // cancel+resubmit per bar for sub-tick moves is churn, not safety.
        const double want = highest_close_ - stop_atr_ * atr_;
        const double step = std::max(0.01, atr_ * 0.05);
        if (stop_id_ && want > stop_px_ + step && ctx.cancel_order(stop_id_)) {
            stop_id_ = 0;
            place_stop(ctx, pos, want);
        }
    }

    int entry_len_ = 20, exit_len_ = 10, atr_len_ = 14;
    double risk_pct_ = 0.5, stop_atr_ = 2.0, max_qty_ = 5000;
    double enter_from_h_ = 0, enter_until_h_ = 24;

    uint32_t sym_ = 0;
    int64_t bar_i_ = 0;
    MonoDeque highs_, lows_;
    double prev_close_ = 0.0, atr_ = 0.0, tr_sum_ = 0.0;
    int tr_n_ = 0;
    uint64_t entry_id_ = 0, stop_id_ = 0, exit_id_ = 0;
    double stop_px_ = 0.0, highest_close_ = 0.0;
};

TT_STRATEGY(DonchianTrendStrategy, "Donchian ATR Trend", kParams)
