// Opening Range Breakout (intraday) — session-aware breakout with bracket
// exits and an end-of-day flatten.
//
// Each session: record the high/low of the first `range_min` minutes, then
// arm a resting Stop-buy at the range high (and optionally a Stop-sell at the
// range low with `allow_short`). On entry, place a protective stop at the far
// side of the range and a take-profit at `tp_r` range-heights from the
// breakout — a manual one-cancels-other pair managed in on_fill, since only
// this strategy knows both order ids. One entry per session; everything is
// flattened `eod_min` minutes before the session ends so nothing rides
// overnight.
//
// Session/day handling is calendar-free on purpose: a "new session" is a new
// UTC day (US RTH sessions never straddle midnight UTC), and time-into-session
// is counted in bars, which makes the logic immune to DST shifts as long as
// the data is regular-hours only.
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace tt;

namespace {
constexpr ParamDesc kParams[] = {
    {"range_min", 15, 1, 120},     // opening range length (minutes)
    {"tp_r", 2.0, 0.5, 10},        // take-profit in range-heights
    {"risk_pct", 1.0, 0.05, 5},    // % of cash risked (stop = far range side)
    {"session_min", 390, 60, 1440},// session length (minutes, 390 = US RTH)
    {"eod_min", 5, 0, 60},         // flatten this early (minutes)
    {"allow_short", 0, 0, 1},      // 1 = also arm the downside break
    {"max_qty", 5000, 1, 100000},  // hard share cap per position
};

constexpr int64_t kDayNs = 86'400'000'000'000;
}

class OrbBreakoutStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        range_min_ = ctx.param("range_min", 15);
        tp_r_ = ctx.param("tp_r", 2.0);
        risk_pct_ = ctx.param("risk_pct", 1.0);
        session_min_ = ctx.param("session_min", 390);
        eod_min_ = ctx.param("eod_min", 5);
        allow_short_ = ctx.param("allow_short", 0) >= 0.5;
        max_qty_ = ctx.param("max_qty", 5000);

        sym_ = 0;
        bar_sec_ = 0;
        prev_ts_ = 0;
        day_ = -1;
        reset_session();

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "ORB: range=%.0fm tp=%.1fR risk=%.2f%% eod=%.0fm short=%d",
                      range_min_, tp_r_, risk_pct_, eod_min_, allow_short_ ? 1 : 0);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        // Bar interval, measured once from the first adjacent pair. Nothing
        // arms until it's known.
        if (bar_sec_ == 0 && prev_ts_ > 0 && bar.ts_ns > prev_ts_)
            bar_sec_ = static_cast<int>((bar.ts_ns - prev_ts_) / 1'000'000'000);
        prev_ts_ = bar.ts_ns;

        const int64_t day = bar.ts_ns / kDayNs;
        if (day != day_) {
            // New session. Leftovers from yesterday (resting orders, an
            // unflattened position) must not leak into today.
            cancel_all(ctx);
            const double pos = ctx.position(sym_).qty;
            if (pos != 0.0 && flatten_id_ == 0)
                flatten_id_ = ctx.submit_order({sym_, pos > 0 ? Side::Sell : Side::Buy,
                                                OrdType::Market, {}, std::abs(pos),
                                                0.0, 0.0, 0.0, 0.0});
            day_ = day;
            reset_session();
        }
        ++bars_today_;

        // End of day: pull every resting order, flatten, stay out.
        if (bar_sec_ > 0) {
            const double minutes = bars_today_ * bar_sec_ / 60.0;
            if (minutes >= session_min_ - eod_min_) {
                if (!eod_done_) {
                    eod_done_ = true;
                    entered_ = true;  // no re-arming today
                    cancel_all(ctx);
                    const double pos = ctx.position(sym_).qty;
                    if (pos != 0.0 && flatten_id_ == 0) {
                        flatten_id_ = ctx.submit_order(
                            {sym_, pos > 0 ? Side::Sell : Side::Buy, OrdType::Market,
                             {}, std::abs(pos), 0.0, 0.0, 0.0, 0.0});
                        ctx.log(1, "EOD flatten");
                    }
                }
                return;
            }
        }

        // Build the opening range.
        const int range_bars =
            bar_sec_ > 0 ? std::max(1, static_cast<int>(range_min_ * 60.0 /
                                                        bar_sec_ + 0.5))
                         : -1;
        if (range_bars < 0 || bars_today_ <= range_bars) {
            range_hi_ = range_hi_ == 0.0 ? bar.high : std::max(range_hi_, bar.high);
            range_lo_ = range_lo_ == 0.0 ? bar.low : std::min(range_lo_, bar.low);
            return;
        }

        // Range complete: arm the breakout stops, once per session.
        if (!armed_ && !entered_ && long_stop_id_ == 0 && short_stop_id_ == 0) {
            armed_ = true;
            const double h = range_hi_ - range_lo_;
            if (h < std::max(0.01, range_hi_ * 1e-6)) return;  // degenerate range
            range_h_ = h;
            const double cash = ctx.cash();
            double qty = std::floor(cash * (risk_pct_ / 100.0) / h);
            qty = std::min(qty, std::floor(cash * 0.95 / range_hi_));
            qty = std::min(qty, max_qty_);
            if (qty < 1.0) return;
            long_stop_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Stop, {},
                                              qty, 0.0, range_hi_, 0.0, 0.0});
            if (allow_short_)
                short_stop_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Stop, {},
                                                   qty, 0.0, range_lo_, 0.0, 0.0});
            char buf[128];
            std::snprintf(buf, sizeof(buf), "range %.2f-%.2f armed, qty %.0f",
                          range_lo_, range_hi_, qty);
            ctx.log(0, buf);
        }
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        char buf[128];
        if (f.order_id == long_stop_id_ || f.order_id == short_stop_id_) {
            const bool went_long = f.order_id == long_stop_id_;
            // Manual OCO on the entries: the untriggered side dies now.
            const uint64_t sibling = went_long ? short_stop_id_ : long_stop_id_;
            if (sibling) ctx.cancel_order(sibling);
            long_stop_id_ = short_stop_id_ = 0;
            entered_ = true;

            // Exit pair: protective stop at the far range side, TP at tp_r
            // range-heights from the breakout. Managed as manual OCO below.
            const double prot = went_long ? range_lo_ : range_hi_;
            const double tp = went_long ? f.price + tp_r_ * range_h_
                                        : f.price - tp_r_ * range_h_;
            const Side exit_side = went_long ? Side::Sell : Side::Buy;
            prot_id_ = ctx.submit_order({sym_, exit_side, OrdType::Stop, {}, f.qty,
                                         0.0, prot, 0.0, 0.0});
            tp_id_ = ctx.submit_order({sym_, exit_side, OrdType::Limit, {}, f.qty,
                                       tp, 0.0, 0.0, 0.0});
            if (prot_id_ == 0)
                ctx.log(3, "protective stop rejected — position unprotected");
            std::snprintf(buf, sizeof(buf), "%s %.0f @ %.2f, stop %.2f tp %.2f",
                          went_long ? "LONG" : "SHORT", f.qty, f.price, prot, tp);
            ctx.log(1, buf);
        } else if (f.order_id == prot_id_) {
            if (tp_id_) ctx.cancel_order(tp_id_);
            prot_id_ = tp_id_ = 0;
            std::snprintf(buf, sizeof(buf), "stopped out @ %.2f", f.price);
            ctx.log(1, buf);
        } else if (f.order_id == tp_id_) {
            if (prot_id_) ctx.cancel_order(prot_id_);
            prot_id_ = tp_id_ = 0;
            std::snprintf(buf, sizeof(buf), "take profit @ %.2f", f.price);
            ctx.log(1, buf);
        } else if (f.order_id == flatten_id_) {
            flatten_id_ = 0;
        }
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "ORB stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    void reset_session() noexcept {
        bars_today_ = 0;
        range_hi_ = range_lo_ = range_h_ = 0.0;
        armed_ = entered_ = eod_done_ = false;
    }

    void cancel_all(IStrategyContext& ctx) noexcept {
        for (uint64_t* id : {&long_stop_id_, &short_stop_id_, &prot_id_, &tp_id_}) {
            if (*id) ctx.cancel_order(*id);
            *id = 0;
        }
    }

    double range_min_ = 15, tp_r_ = 2.0, risk_pct_ = 1.0;
    double session_min_ = 390, eod_min_ = 5, max_qty_ = 5000;
    bool allow_short_ = false;

    uint32_t sym_ = 0;
    int bar_sec_ = 0;
    int64_t prev_ts_ = 0, day_ = -1;
    int bars_today_ = 0;
    double range_hi_ = 0.0, range_lo_ = 0.0, range_h_ = 0.0;
    bool armed_ = false, entered_ = false, eod_done_ = false;
    uint64_t long_stop_id_ = 0, short_stop_id_ = 0;
    uint64_t prot_id_ = 0, tp_id_ = 0, flatten_id_ = 0;
};

TT_STRATEGY(OrbBreakoutStrategy, "Opening Range Breakout", kParams)
