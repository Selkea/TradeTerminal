// Bollinger Z-Score Reversion (long/flat) — band mean reversion.
//
// Entry:  z-score of close vs the `length`-bar mean drops to -`entry_z` or
//         below, optionally gated by a long-term trend filter (only buy dips
//         in an uptrend — mean reversion against the trend is how these
//         systems blow up).
// Exit:   z recovers to -`exit_z` AND the price is above the entry (the
//         reversion happened), or the position ages past `time_stop` bars (it
//         didn't — take the loss and move on). Deliberately NO price stop:
//         hard stops on mean-reversion entries systematically sell the lows;
//         the time stop is the risk control, and position size is the loss
//         limiter. `time_stop` is therefore MANDATORY — with no price stop it
//         is the only thing that ever closes a losing position.
//
// Why the exit checks the price and not just z: z is a distance from a ROLLING
// mean, so it recovers two ways — the price rises back to the average, or the
// average falls to meet the price. Only the first is a reversion. The second is
// the trade failing, and on a short `length` the mean catches up within a few
// bars, so the z test alone fires while the position is underwater and books
// the loss as though the thesis had worked. Observed live 2026-08-05: SPCH
// bought 770 @ 6.49 on a `length`-5 z-score, price fell 6.3%, and 50 minutes
// later it exited "reverted to band" at 6.08 for -$315.70 — the entire day's
// loss on that symbol, from one trade the strategy reported as a success.
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
    // Max bars in trade. Minimum 1, not 0: with no price stop this is the only
    // exit a losing position has, and the optimizer swept it to 0 on SPCH —
    // leaving a position with no risk control of any kind.
    {"time_stop", 12, 1, 500},
    {"trend_len", 200, 0, 1000},   // SMA trend filter (0 = off)
    {"budget_pct", 100, 1, 100},   // % of the risk budget per entry (ctx.budget)
    {"max_qty", 5000, 1, 100000},  // hard share cap per position
    // Entry window, local hours (9.5 = 09:30). 0/24 = always; exits not gated.
    {"enter_from_h", 0, 0, 24},
    {"enter_until_h", 24, 0, 24},
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
        budget_pct_ = ctx.param("budget_pct", 100);
        max_qty_ = ctx.param("max_qty", 5000);
        enter_from_h_ = ctx.param("enter_from_h", 0);
        enter_until_h_ = ctx.param("enter_until_h", 24);
        if (length_ < 2) length_ = 2;
        if (exit_z_ > entry_z_) exit_z_ = entry_z_;  // exit band inside entry band
        // Repair a stored 0 from before the minimum was raised. Left at 0 the
        // position has no time stop and no price stop — nothing closes it at a
        // loss at all.
        if (time_stop_ < 1) {
            time_stop_ = 12;
            ctx.log(2, "time_stop was 0 (no risk control) — restored to 12 bars");
        }

        sym_ = 0;
        closes_.clear();
        closes_.reserve(1 << 20);
        sum_ = sum2_ = trend_sum_ = 0.0;
        entry_id_ = exit_id_ = 0;
        bars_held_ = 0;
        entry_px_ = 0.0;

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
        // A dead tape (every close identical) has no meaningful z-score. This
        // used to `return` here — which also skipped the position block below,
        // so a pinned price froze the TIME STOP and an open position could not
        // be closed at all. Only the entry needs a live z; risk management runs
        // regardless.
        const bool live_z = sd >= 1e-12;
        const double z = live_z ? (bar.close - mean) / sd : 0.0;

        const double pos = ctx.position(sym_).qty;
        if (pos > 0.0) {
            ++bars_held_;
            if (exit_id_ != 0) return;  // exit already in flight
            // A reversion is the PRICE coming back, not the mean coming down to
            // meet it (see the header). entry_px_ 0 means the position was
            // adopted at session start with no fill of ours behind it — no
            // reference to judge against, so fall back to the z test alone.
            const bool recovered = entry_px_ <= 0.0 || bar.close > entry_px_;
            const bool reverted = live_z && z >= -exit_z_ && recovered;
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
        if (!live_z) return;                          // no z: nothing to enter on
        if (entry_id_ != 0 || exit_id_ != 0) return;  // an order is in flight
        const bool trend_ok =
            trend_len_ == 0 || bar.close > trend_sum_ / trend_len_;
        const double hod = hour_of_day_local(bar.ts_ns);
        const bool time_ok = hod >= enter_from_h_ && hod < enter_until_h_;
        if (z <= -entry_z_ && trend_ok && time_ok) {
            // Size off the risk budget, not cash: the engine caps the position
            // notional anyway, so a % of the account is a number it discards.
            const double budget = ctx.budget(sym_);
            double qty = std::floor(budget * (budget_pct_ / 100.0) / bar.close);
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
            entry_px_ = f.price;   // the level a reversion has to get back to
        } else if (f.order_id == exit_id_) {
            exit_id_ = 0;
            entry_px_ = 0.0;
        }
    }

    // Without this, one rejected order leaves entry_id_ set and the in-flight
    // guard above blocks every later entry for the rest of the session.
    void on_order_end(IStrategyContext&, const OrderEnd& e) noexcept override {
        if (e.order_id == entry_id_) entry_id_ = 0;   // else the in-flight guard
        else if (e.order_id == exit_id_) exit_id_ = 0;  // wedges the symbol
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "BollRev stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    int length_ = 20, time_stop_ = 12, trend_len_ = 200;
    double entry_z_ = 2.0, exit_z_ = 0.25, budget_pct_ = 100, max_qty_ = 5000;
    double enter_from_h_ = 0, enter_until_h_ = 24;

    uint32_t sym_ = 0;
    std::vector<double> closes_;
    double sum_ = 0.0, sum2_ = 0.0, trend_sum_ = 0.0;
    uint64_t entry_id_ = 0, exit_id_ = 0;
    double entry_px_ = 0.0;   // fill price of the open position (0 = none/adopted)
    int bars_held_ = 0;
};

TT_STRATEGY(BollingerReversionStrategy, "Bollinger Reversion", kParams)
