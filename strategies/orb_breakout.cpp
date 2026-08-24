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
    // % of CASH risked (stop = far range side). INERT on any account big enough
    // for the caps to bind, and kept only for the ones where it still moves:
    // against a per-symbol notional cap and a per-trade risk budget, cash is the
    // loosest of the three bounds, so on the $1M paper account every value from
    // ~0.1 up sizes identically. The 1.0 here is this default, not a fitted
    // number -- risk_pct is in tt::ui::sweep_param_is_fixed, so no sweep has ever
    // moved it. Anything that needs to bound a loss must use ctx.risk_budget().
    {"risk_pct", 1.0, 0.05, 5},
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
        // Risk overlay injected by the app in "hold — don't halt" mode: don't let
        // the EOD / new-day housekeeping flatten an underwater position.
        hold_losers_ = ctx.param("__hold_losers", 0) >= 0.5;

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
            const Position p = ctx.position(sym_);
            const bool hold_loser = hold_losers_ && p.qty != 0.0 && p.unrealized_pnl < 0.0;
            cancel_all(ctx);
            if (!hold_loser && p.qty != 0.0 && flatten_id_ == 0)
                flatten_id_ = ctx.submit_order({sym_, p.qty > 0 ? Side::Sell : Side::Buy,
                                                OrdType::Market, {}, std::abs(p.qty),
                                                0.0, 0.0, 0.0, 0.0});
            day_ = day;
            reset_session();
            if (hold_loser) {
                // Hold the underwater position (no stop) until it recovers; don't
                // arm a new breakout on top of it this session.
                entered_ = true;
                ctx.log(1, "hold mode: keeping underwater position, no re-entry today");
            }
        }
        ++bars_today_;

        // End of day: pull every resting order, flatten, stay out.
        if (bar_sec_ > 0) {
            const double minutes = bars_today_ * bar_sec_ / 60.0;
            // Counted bars OR the clock. The count is only as good as
            // session_min: set above the real session length it never trips and
            // the position rides overnight — an intraday strategy holding
            // through the close, from one bad parameter. The clock backstop
            // cannot be pushed past the close by any value. Intraday bars only
            // (a daily bar has no meaningful time of day), and it only ever
            // ADDS a flatten, so a session that legitimately ends earlier is
            // unaffected. Assumes an Eastern-time box, as US RTH gates do.
            const bool by_clock =
                bar_sec_ < 23 * 3600 && hour_of_day_local(bar.ts_ns) >= kEodBackstopH;
            if (minutes >= session_min_ - eod_min_ || by_clock) {
                if (!eod_done_) {
                    eod_done_ = true;
                    entered_ = true;  // no re-arming today
                    const Position p = ctx.position(sym_);
                    const bool hold_loser =
                        hold_losers_ && p.qty != 0.0 && p.unrealized_pnl < 0.0;
                    cancel_all(ctx);
                    if (!hold_loser && p.qty != 0.0 && flatten_id_ == 0) {
                        flatten_id_ = ctx.submit_order(
                            {sym_, p.qty > 0 ? Side::Sell : Side::Buy, OrdType::Market,
                             {}, std::abs(p.qty), 0.0, 0.0, 0.0, 0.0});
                        ctx.log(1, "EOD flatten");
                    } else if (hold_loser) {
                        ctx.log(1, "hold mode: keeping underwater position overnight");
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
            // THE RISK CEILING, and the only one of the three that knows what
            // this trade can actually lose. The protective stop goes at the far
            // side of the range (see enter_bracket), so `h` is not an assumed
            // excursion — it is the exact per-share loss if the stop fills.
            //
            // This is what risk_pct_ above was trying to compute and could not:
            // it is a percentage of CASH, and on a $1M paper account against a
            // $2k daily limit every value it can be swept to means the same
            // thing (see the ParamDesc note). Left in place rather than removed
            // because it still binds on an account small enough for it to matter
            // — but it is no longer the thing standing between a wide stop and
            // the day's loss budget.
            const double risk_dollars = ctx.risk_budget(sym_);
            if (risk_dollars > 0.0)
                qty = std::min(qty, std::floor(risk_dollars / h));
            // Notional bound is the risk budget, not the raw balance: the engine
            // caps the position there anyway, and the tp/stop legs below are
            // sized off this qty, so they must match what will actually fill.
            qty = std::min(qty, std::floor(ctx.budget(sym_) / range_hi_));
            qty = std::min(qty, max_qty_);
            if (qty < 1.0) return;
            long_stop_id_ = ctx.submit_order({sym_, Side::Buy, OrdType::Stop, {},
                                              qty, 0.0, range_hi_, 0.0, 0.0});
            if (allow_short_)
                short_stop_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Stop, {},
                                                   qty, 0.0, range_lo_, 0.0, 0.0});
            char buf[160];
            // Report the RISK, not just the size. A breakout arms hours before it
            // can fill, so this line is the only chance to see what the trade
            // would cost before it costs it — 2026-08-24's CAPR armed a 20.7%
            // stop and nothing said so until the numbers were worked out by hand.
            std::snprintf(buf, sizeof(buf),
                          "range %.2f-%.2f armed, qty %.0f, risking $%.0f "
                          "(stop %.1f%% away)",
                          range_lo_, range_hi_, qty, qty * h,
                          range_hi_ > 0.0 ? 100.0 * h / range_hi_ : 0.0);
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
            // KEEP the entry id (in entry_ord_): a marketable breakout order
            // fills in several partials and every one of them has to grow the
            // bracket. Zeroing it here was what left 426 of SNXX's 526 shares
            // naked on 2026-08-06 — see arm_bracket.
            entry_ord_ = f.order_id;
            long_stop_id_ = short_stop_id_ = 0;
            entered_ = true;
            arm_bracket(ctx);
            std::snprintf(buf, sizeof(buf), "%s %.0f @ %.2f, stop %.2f tp %.2f",
                          went_long ? "LONG" : "SHORT", bracket_qty_, f.price,
                          prot_px_, tp_px_);
            ctx.log(1, buf);
        } else if (f.order_id == entry_ord_) {
            // A later partial of the same entry: the position just grew.
            arm_bracket(ctx);
            std::snprintf(buf, sizeof(buf), "entry filled to %.0f, bracket resized",
                          bracket_qty_);
            ctx.log(0, buf);
        } else if (f.order_id == prot_id_ || f.order_id == tp_id_) {
            const bool stopped = f.order_id == prot_id_;
            if (std::abs(ctx.position(sym_).qty) > 0.0) {
                // Partial exit. The filled leg still works its own remainder,
                // but the sibling is now oversized — completing it would flip
                // the position instead of closing it. Re-place it at live size.
                resize_sibling(ctx, stopped);
                return;
            }
            const uint64_t sibling = stopped ? tp_id_ : prot_id_;
            if (sibling) ctx.cancel_order(sibling);
            prot_id_ = tp_id_ = entry_ord_ = 0;
            bracket_qty_ = 0.0;
            std::snprintf(buf, sizeof(buf), "%s @ %.2f",
                          stopped ? "stopped out" : "take profit", f.price);
            ctx.log(1, buf);
        } else if (f.order_id == flatten_id_) {
            flatten_id_ = 0;
        }
    }

    void on_order_end(IStrategyContext& ctx, const OrderEnd& e) noexcept override {
        if (e.order_id == long_stop_id_ || e.order_id == short_stop_id_) {
            if (e.order_id == long_stop_id_) long_stop_id_ = 0;
            else short_stop_id_ = 0;
            // Both breakout stops died without ever triggering. armed_ latches
            // for the session, so this is the difference between "ORB missed
            // one entry" and "ORB produces nothing for the rest of the day".
            // Bounded: a symbol that rejects every time must not be retried on
            // every bar until the close.
            if (!entered_ && !eod_done_ && long_stop_id_ == 0 &&
                short_stop_id_ == 0 && rearms_ < kMaxRearms) {
                ++rearms_;
                armed_ = false;
                ctx.log(2, "breakout orders died before triggering — re-arming");
            }
        } else if (e.order_id == prot_id_) {
            prot_id_ = 0;
            // A REJECTED protective stop leaves the position naked; re-arm at
            // the same far range side. A cancelled one is cancel_all/OCO.
            if (e.reason == OrderEndReason::Rejected &&
                ctx.position(sym_).qty != 0.0)
                arm_bracket(ctx);
        } else if (e.order_id == tp_id_) {
            tp_id_ = 0;
        } else if (e.order_id == flatten_id_) {
            flatten_id_ = 0;
            if (ctx.position(sym_).qty != 0.0)
                ctx.log(3, "EOD flatten died — position still open");
        }
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "ORB stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    // How many times a session may re-arm after its breakout orders died
    // without triggering (see on_order_end).
    static constexpr int kMaxRearms = 2;

    // Hard EOD flatten time (local), regardless of session_min/eod_min: 15:54,
    // matching the default eod_min of 5 minutes before a 16:00 US close.
    static constexpr double kEodBackstopH = 15.9;

    void reset_session() noexcept {
        bars_today_ = 0;
        range_hi_ = range_lo_ = range_h_ = 0.0;
        armed_ = entered_ = eod_done_ = false;
        rearms_ = 0;
        entry_ord_ = 0;
        bracket_qty_ = 0.0;
    }

    void cancel_all(IStrategyContext& ctx) noexcept {
        for (uint64_t* id : {&long_stop_id_, &short_stop_id_, &prot_id_, &tp_id_}) {
            if (*id) ctx.cancel_order(*id);
            *id = 0;
        }
        entry_ord_ = 0;
        bracket_qty_ = 0.0;
    }

    // (Re)arm the protective pair so it covers the ENTIRE live position.
    //
    // Sizing off the fill is the trap: a marketable entry arrives as several
    // partials, so the first one covers a fraction of what ends up open and
    // the rest of the position never gets a stop at all. Size off the position
    // and re-arm as each partial lands. Idempotent — a call that finds the
    // bracket already the right size does nothing, so the common
    // single-print fill still places exactly two orders.
    void arm_bracket(IStrategyContext& ctx) noexcept {
        const Position p = ctx.position(sym_);
        const double qty = std::abs(p.qty);
        if (qty <= 0.0) return;
        if (prot_id_ != 0 && tp_id_ != 0 && qty == bracket_qty_) return;

        const bool went_long = p.qty > 0.0;
        // TP keys off the position's AVERAGE entry, not the first print, so a
        // bracket rebuilt mid-fill targets the same edge the whole way up.
        prot_px_ = went_long ? range_lo_ : range_hi_;
        tp_px_ = went_long ? p.avg_price + tp_r_ * range_h_
                           : p.avg_price - tp_r_ * range_h_;
        const Side exit_side = went_long ? Side::Sell : Side::Buy;

        if (prot_id_) ctx.cancel_order(prot_id_);
        if (tp_id_) ctx.cancel_order(tp_id_);
        prot_id_ = ctx.submit_order({sym_, exit_side, OrdType::Stop, {}, qty, 0.0,
                                     prot_px_, 0.0, 0.0});
        tp_id_ = ctx.submit_order({sym_, exit_side, OrdType::Limit, {}, qty,
                                   tp_px_, 0.0, 0.0, 0.0});
        bracket_qty_ = qty;
        if (prot_id_ == 0)
            ctx.log(3, "protective stop rejected — position unprotected");
    }

    // One protective leg partially filled: shrink the other to match what is
    // actually still open. The filled leg is left alone — it is already
    // working its own remainder, and cancelling a triggered stop races it.
    void resize_sibling(IStrategyContext& ctx, bool stopped) noexcept {
        const Position p = ctx.position(sym_);
        const double qty = std::abs(p.qty);
        uint64_t& sib = stopped ? tp_id_ : prot_id_;
        if (sib == 0 || qty <= 0.0) return;
        ctx.cancel_order(sib);
        const Side exit_side = p.qty > 0.0 ? Side::Sell : Side::Buy;
        sib = stopped ? ctx.submit_order({sym_, exit_side, OrdType::Limit, {}, qty,
                                          tp_px_, 0.0, 0.0, 0.0})
                      : ctx.submit_order({sym_, exit_side, OrdType::Stop, {}, qty,
                                          0.0, prot_px_, 0.0, 0.0});
        bracket_qty_ = qty;
    }

    double range_min_ = 15, tp_r_ = 2.0, risk_pct_ = 1.0;
    double session_min_ = 390, eod_min_ = 5, max_qty_ = 5000;
    bool allow_short_ = false;
    bool hold_losers_ = false;   // hold-mode overlay: keep underwater positions

    uint32_t sym_ = 0;
    int bar_sec_ = 0;
    int64_t prev_ts_ = 0, day_ = -1;
    int bars_today_ = 0;
    double range_hi_ = 0.0, range_lo_ = 0.0, range_h_ = 0.0;
    bool armed_ = false, entered_ = false, eod_done_ = false;
    int rearms_ = 0;
    uint64_t long_stop_id_ = 0, short_stop_id_ = 0;
    uint64_t prot_id_ = 0, tp_id_ = 0, flatten_id_ = 0;
    // The triggered entry order, kept alive across its partials so each one
    // can re-arm the bracket; and what that bracket currently covers.
    uint64_t entry_ord_ = 0;
    double bracket_qty_ = 0.0, prot_px_ = 0.0, tp_px_ = 0.0;
};

TT_STRATEGY(OrbBreakoutStrategy, "Opening Range Breakout", kParams)
