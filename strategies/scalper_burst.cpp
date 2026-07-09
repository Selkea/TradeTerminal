// Momentum-burst scalper (tick-driven) — enters when price moves burst_bps
// within a rolling window_s-second window of real trade prints, rides the
// continuation, and exits via its own TP-limit / SL-stop pair (manual OCO), a
// hold_max_s time-stop, or nothing at all: no signal, no trade. A cooldown_s
// pause follows every exit so the strategy cannot machine-gun one symbol.
//
// Scalping economics are unforgiving: the sim charges $0.005/share (min $1)
// per order plus slippage, so tp_bps must clear roundtrip costs on the chosen
// size to break even — on a $200 stock, ~2 bp of fees + ~2 bp slippage means
// tp_bps below ~6 rarely survives. Validate with Record -> Replay (tick
// level); candle backtests cannot see inside a bar and will grade this
// strategy meaninglessly.
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <utility>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"window_s", 3, 1, 60},        // burst lookback (seconds)
    {"burst_bps", 8, 1, 100},      // move within the window that triggers entry
    {"min_ticks", 5, 2, 200},      // min prints in the window (sparse tape = no go)
    {"tp_bps", 10, 1, 200},        // take-profit distance
    {"sl_bps", 8, 1, 200},         // protective-stop distance
    {"hold_max_s", 30, 1, 600},    // time-stop: flatten after this long in a trade
    {"cooldown_s", 10, 0, 600},    // pause after every exit
    {"allow_short", 0, 0, 1},      // 1 = also scalp downside bursts
    {"alloc_pct", 25, 1, 100},     // % of cash per entry (sizing: not optimized)
    {"max_qty", 2000, 1, 100000},  // hard share cap per position
    // Entry window, local hours (9.5 = 09:30). 0/24 = always. Exits are never
    // gated. from >= until disables entries entirely.
    {"enter_from_h", 0, 0, 24},
    {"enter_until_h", 24, 0, 24},
};
}

class ScalperBurstStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        window_s_ = ctx.param("window_s", 3);
        burst_bps_ = ctx.param("burst_bps", 8);
        min_ticks_ = static_cast<int>(ctx.param("min_ticks", 5));
        tp_bps_ = ctx.param("tp_bps", 10);
        sl_bps_ = ctx.param("sl_bps", 8);
        hold_max_s_ = ctx.param("hold_max_s", 30);
        cooldown_s_ = ctx.param("cooldown_s", 10);
        allow_short_ = ctx.param("allow_short", 0) >= 0.5;
        alloc_pct_ = ctx.param("alloc_pct", 25);
        max_qty_ = ctx.param("max_qty", 2000);
        enter_from_h_ = ctx.param("enter_from_h", 0);
        enter_until_h_ = ctx.param("enter_until_h", 24);

        sym_ = 0;
        window_.clear();
        entry_id_ = tp_id_ = sl_id_ = flatten_id_ = 0;
        entered_ns_ = 0;
        cooldown_until_ns_ = 0;

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Scalper: %.0fs/%.0fbps burst -> tp %.0f sl %.0f bps, "
                      "hold<=%.0fs cooldown %.0fs short=%d",
                      window_s_, burst_bps_, tp_bps_, sl_bps_, hold_max_s_,
                      cooldown_s_, allow_short_ ? 1 : 0);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}

    void on_tick(IStrategyContext& ctx, uint32_t symbol_id, const Tick& tick) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_ || tick.price <= 0.0) return;
        const int64_t now = ctx.now_ns();

        // Rolling window of recent prints.
        window_.emplace_back(now, tick.price);
        const int64_t horizon = now - static_cast<int64_t>(window_s_ * 1e9);
        while (!window_.empty() && window_.front().first < horizon) window_.pop_front();

        const double pos = ctx.position(sym_).qty;

        // In a trade: enforce the time-stop; exits themselves are resting orders.
        if (pos != 0.0) {
            if (entered_ns_ > 0 && flatten_id_ == 0 &&
                now - entered_ns_ >= static_cast<int64_t>(hold_max_s_ * 1e9)) {
                cancel_exits(ctx);
                flatten_id_ = ctx.submit_order({sym_, pos > 0 ? Side::Sell : Side::Buy,
                                                OrdType::Market, {}, std::abs(pos),
                                                0.0, 0.0, 0.0, 0.0});
                ctx.log(1, "time-stop flatten");
            }
            return;
        }
        if (entry_id_ != 0 || flatten_id_ != 0) return;   // an order is in flight
        if (now < cooldown_until_ns_) return;
        if (static_cast<int>(window_.size()) < min_ticks_) return;

        // Burst: move from the oldest print in the window to this one.
        const double base = window_.front().second;
        if (base <= 0.0) return;
        const double move_bps = (tick.price - base) / base * 10'000.0;
        const bool up = move_bps >= burst_bps_;
        const bool down = allow_short_ && move_bps <= -burst_bps_;
        if (!up && !down) return;

        // Time-of-day gate on new entries only (exits above are never gated).
        const double hod = hour_of_day_local(now);
        if (hod < enter_from_h_ || hod >= enter_until_h_) return;

        double qty = std::floor(ctx.cash() * (alloc_pct_ / 100.0) / tick.price);
        qty = std::min(qty, max_qty_);
        if (qty < 1.0) return;

        entry_long_ = up;
        entry_id_ = ctx.submit_order({sym_, up ? Side::Buy : Side::Sell,
                                      OrdType::Market, {}, qty, 0.0, 0.0, 0.0, 0.0});
        if (entry_id_ == 0) {
            // Rejected (risk/buying power): stand down for one cooldown.
            cooldown_until_ns_ = now + static_cast<int64_t>(cooldown_s_ * 1e9);
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "burst %+.1f bps -> %s %.0f", move_bps,
                      up ? "BUY" : "SELL", qty);
        ctx.log(0, buf);
    }

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        char buf[128];
        if (f.order_id == entry_id_) {
            entry_id_ = 0;
            entered_ns_ = ctx.now_ns();
            // Exit pair around the actual fill price (manual OCO in on_fill).
            const double tp = entry_long_ ? f.price * (1.0 + tp_bps_ / 10'000.0)
                                          : f.price * (1.0 - tp_bps_ / 10'000.0);
            const double sl = entry_long_ ? f.price * (1.0 - sl_bps_ / 10'000.0)
                                          : f.price * (1.0 + sl_bps_ / 10'000.0);
            const Side exit_side = entry_long_ ? Side::Sell : Side::Buy;
            sl_id_ = ctx.submit_order({sym_, exit_side, OrdType::Stop, {}, f.qty,
                                       0.0, sl, 0.0, 0.0});
            tp_id_ = ctx.submit_order({sym_, exit_side, OrdType::Limit, {}, f.qty,
                                       tp, 0.0, 0.0, 0.0});
            if (sl_id_ == 0)
                ctx.log(3, "protective stop rejected — position unprotected");
            std::snprintf(buf, sizeof(buf), "in %.0f @ %.2f, tp %.2f sl %.2f", f.qty,
                          f.price, tp, sl);
            ctx.log(1, buf);
        } else if (f.order_id == sl_id_ || f.order_id == tp_id_) {
            const bool stopped = f.order_id == sl_id_;
            const uint64_t sibling = stopped ? tp_id_ : sl_id_;
            if (sibling) ctx.cancel_order(sibling);
            tp_id_ = sl_id_ = 0;
            entered_ns_ = 0;
            cooldown_until_ns_ =
                ctx.now_ns() + static_cast<int64_t>(cooldown_s_ * 1e9);
            std::snprintf(buf, sizeof(buf), "%s @ %.2f",
                          stopped ? "stopped" : "take profit", f.price);
            ctx.log(1, buf);
        } else if (f.order_id == flatten_id_) {
            flatten_id_ = 0;
            entered_ns_ = 0;
            cooldown_until_ns_ =
                ctx.now_ns() + static_cast<int64_t>(cooldown_s_ * 1e9);
        }
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "Scalper stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    void cancel_exits(IStrategyContext& ctx) noexcept {
        for (uint64_t* id : {&tp_id_, &sl_id_}) {
            if (*id) ctx.cancel_order(*id);
            *id = 0;
        }
    }

    double window_s_ = 3, burst_bps_ = 8, tp_bps_ = 10, sl_bps_ = 8;
    double hold_max_s_ = 30, cooldown_s_ = 10, alloc_pct_ = 25, max_qty_ = 2000;
    double enter_from_h_ = 0, enter_until_h_ = 24;
    int min_ticks_ = 5;
    bool allow_short_ = false;

    uint32_t sym_ = 0;
    std::deque<std::pair<int64_t, double>> window_;   // (engine ns, price)
    bool entry_long_ = false;
    uint64_t entry_id_ = 0, tp_id_ = 0, sl_id_ = 0, flatten_id_ = 0;
    int64_t entered_ns_ = 0, cooldown_until_ns_ = 0;
};

TT_STRATEGY(ScalperBurstStrategy, "Momentum Scalper", kParams)
