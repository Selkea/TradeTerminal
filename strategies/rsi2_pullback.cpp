// RSI-2 Pullback (long/flat) — the Connors short-term pullback system.
//
// Buy weakness inside an uptrend, sell the first strength that is worth the
// fees:
//   - regime filter: only long while close > SMA(`trend_ma`)
//   - entry: Wilder RSI(`rsi_len`) at or below `buy_below`
//   - exit: close crosses above SMA(`exit_ma`) AND the trade is at least
//     `min_gain_cps` cents/share above OUR entry — or the position ages past
//     `time_stop` bars, which is never gated on profit.
// No price stop, by design: the edge is buying short-term panic, and stops
// placed inside the panic get hit systematically. The regime filter plus the
// small fixed allocation are the risk controls (Connors' published results
// degrade with stops attached — keep size modest instead). That makes
// `time_stop` MANDATORY for the same reason it is in bollinger_reversion: it
// is the only thing that ever closes a position that does not work out.
//
// Why the exit carries a P&L term: `close > SMA(exit_ma)` is a statement about
// the last few bars, not about the trade. It fires just as readily one tick
// above the entry as ten cents above, and IBKR charges about half a cent per
// share either way. SPCH, 2026-08-07: 8 round trips, $88.11 gross, $53.54
// commission — 60.8% of gross — on $79,986 of turnover against a position that
// never exceeded ~$5,000, for a net of $34.57. Break-even was ~1.05 c/share and
// the mean gross capture was 1.71. The clearest case bought 642 @ 7.78 at
// 13:30:13 and sold the same 642 @ 7.78 ten minutes later: gross exactly $0.00,
// $6.65 paid. `min_gain_cps` is the fee term the MA test never had.
//
// Why `exit_ma` has a floor of 4: the current bar sits inside the average, so
// the test reduces exactly. close[t] > (1/n)·Σ_{i=0..n-1} close[t-i] is
// (n-1)·close[t] > Σ_{i=1..n-1} close[t-i], i.e. close[t] > mean of the PREVIOUS
// n-1 closes — the effective smoothing is n-1, not n. At n=2 that is
// `close[t] > close[t-1]`: exit on any single up bar, a coin flip that pays
// commission, and exactly where the optimizer pinned SPCH. n=3 is a two-bar
// mean, which on a 5-minute tape still flips on most up bars; 4 is the smallest
// value whose average spans three prior bars.
//
// See strategies/README.md for the SDK rules this file follows.

#include "tt/strategy_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tt;

namespace {
// Ceiling for min_gain_cps, shared by the declared range and the repair in
// on_init (see both for the 2026-08-10 optimizer pinning that motivates it).
constexpr double kMaxGainCps = 10;

constexpr ParamDesc kParams[] = {
    {"rsi_len", 2, 2, 14},          // RSI period (Wilder)
    {"buy_below", 10, 1, 40},       // entry threshold
    // Exit SMA (bars). Minimum 4, not 2: the test reduces to a mean of the
    // previous exit_ma-1 closes, so 2 is "any up bar" and 3 is a two-bar mean
    // (see the header). The optimizer pinned SPCH to the old floor of 2.
    {"exit_ma", 5, 4, 50},
    {"trend_ma", 200, 20, 1000},    // regime SMA (bars)
    // Minimum gain over OUR entry, in cents per share, before the MA exit may
    // fire. Cents per share rather than percent because that is the shape of
    // the cost being covered: IBKR bills per share and does not care what the
    // share costs, which is why a day of 8 trades on a $7.78 stock handed 60.8%
    // of its gross to fees. Measured round-trip cost was ~1.05 c/share, so the
    // floor of 2 is the cheapest setting that still clears fees.
    //
    // Ceiling 10, not 100. The old ceiling assumed the optimizer would pin this
    // to the FLOOR the way it did exit_ma; it did the opposite. Every one of the
    // 13 winning runs on 2026-08-10 came back 69.93-100 c/share (SNXX at exactly
    // 100 in 5 of 6), because in-sample a high bar simply selects the winners.
    // A $1.00/share gate on a $7 stock is not a cost floor, it is a fantasy
    // profit target, and it hands every exit to the time stop. This parameter is
    // no longer swept at all (sweep_param_is_fixed in panels/sweep.h), so the
    // range now only bounds what a human types: 10 c/share is ~5x the measured
    // round trip and ~5x the worst case IBKR's $1 order minimum can produce on a
    // 100-share round trip, while on the $7-8 names this trades it is already
    // 1.3% a trade — past that it is a target, not a cost.
    {"min_gain_cps", 3, 2, kMaxGainCps},
    // Max bars in trade, and the only exit a losing position has. MANDATORY
    // (minimum 1, never 0) — with no price stop and a profit gate on the MA
    // exit, a trade that never gets min_gain_cps above entry has nothing else
    // to close it. Sized as a backstop, not an exit: 24 bars is two hours on
    // the 5-minute bars this runs live, because a time stop short enough to
    // fire often would just re-create the churn this gate is removing.
    {"time_stop", 24, 1, 500},
    {"budget_pct", 100, 1, 100},    // % of the risk budget per entry (ctx.budget)
    {"max_qty", 5000, 1, 100000},   // hard share cap per position
    // Entry window, local hours (9.5 = 09:30). Exits are never gated, and the
    // window is ignored on daily+ bars (see on_bar). Defaults to US regular
    // hours rather than the always-on 0/24 the other strategies declare: a 5m
    // bar built from thin PRE-MARKET prints is exactly what this strategy's
    // "buy the panic" entry mistakes for a signal. On 2026-08-04 it fired at
    // 04:37; IBKR held the order PreSubmitted for five hours and filled it at
    // the open, at a price the strategy never evaluated.
    {"enter_from_h", 9.5, 0, 24},
    {"enter_until_h", 16, 0, 24},
};
}

class Rsi2PullbackStrategy final : public IStrategy {
public:
    void on_init(IStrategyContext& ctx) noexcept override {
        rsi_len_ = static_cast<int>(ctx.param("rsi_len", 2));
        buy_below_ = ctx.param("buy_below", 10);
        exit_ma_ = static_cast<int>(ctx.param("exit_ma", 5));
        trend_ma_ = static_cast<int>(ctx.param("trend_ma", 200));
        min_gain_cps_ = ctx.param("min_gain_cps", 3);
        time_stop_ = static_cast<int>(ctx.param("time_stop", 24));
        budget_pct_ = ctx.param("budget_pct", 100);
        max_qty_ = ctx.param("max_qty", 5000);
        enter_from_h_ = ctx.param("enter_from_h", 9.5);
        enter_until_h_ = ctx.param("enter_until_h", 16);
        bar_sec_ = 0;
        prev_ts_ = 0;
        if (rsi_len_ < 2) rsi_len_ = 2;
        // Repair a stored value from before the minimum was raised. Left at 2
        // the exit is not a moving average at all — it is close[t] > close[t-1],
        // one up bar, which is where the optimizer parked SPCH.
        if (exit_ma_ < 4) {
            char fix[96];
            std::snprintf(fix, sizeof(fix),
                          "exit_ma was %d (exit on any up bar) — restored to 5 bars",
                          exit_ma_);
            exit_ma_ = 5;
            ctx.log(2, fix);
        }
        // Same repair, same reason as bollinger_reversion's: with no price stop
        // and a profit gate on the MA exit, 0 leaves a losing position with
        // nothing to close it at all.
        if (time_stop_ < 1) {
            time_stop_ = 24;
            ctx.log(2, "time_stop was 0 (no risk control) — restored to 24 bars");
        }
        if (min_gain_cps_ < 0.0) min_gain_cps_ = 0.0;   // a negative gate is no gate
        // Repair a value the optimizer swept in before this became a fixed
        // parameter, the same way exit_ma=2 is repaired above. Nothing clamps a
        // stored param to its declared range at load, so the 69.93-100 c/share
        // settings the 2026-08-10 tournaments wrote into the live lineup would
        // otherwise survive every future optimization untouched — fixing the
        // sweep stops new ones, this undoes the ones already saved.
        if (min_gain_cps_ > kMaxGainCps) {
            char fix[112];
            std::snprintf(fix, sizeof(fix),
                          "min_gain_cps was %.2fc (a profit target, not a cost "
                          "floor) — restored to 3c",
                          min_gain_cps_);
            min_gain_cps_ = 3;
            ctx.log(2, fix);
        }
        if (trend_ma_ < exit_ma_) trend_ma_ = exit_ma_;

        sym_ = 0;
        closes_.clear();
        closes_.reserve(1 << 20);
        exit_sum_ = trend_sum_ = 0.0;
        prev_close_ = 0.0;
        avg_gain_ = avg_loss_ = 0.0;
        rsi_n_ = 0;
        entry_id_ = exit_id_ = 0;
        entry_px_ = 0.0;
        bars_held_ = 0;

        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "RSI2: len=%d buy<%.0f exitMA=%d trendMA=%d gain>=%.1fc "
                      "tstop=%d budget=%.0f%%",
                      rsi_len_, buy_below_, exit_ma_, trend_ma_, min_gain_cps_,
                      time_stop_, budget_pct_);
        ctx.log(1, buf);
    }

    void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept override {
        if (sym_ == 0) sym_ = symbol_id;
        if (symbol_id != sym_) return;

        // Bar size, inferred from the first gap (the strategy is never told it).
        if (bar_sec_ == 0 && prev_ts_ > 0 && bar.ts_ns > prev_ts_)
            bar_sec_ = static_cast<int>((bar.ts_ns - prev_ts_) / 1'000'000'000);
        prev_ts_ = bar.ts_ns;

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

        // Position management runs BEFORE the warmup gate, and that gate now
        // holds back only the ENTRY. It used to sit above this block, so an open
        // position had no exit of any kind until trend_ma bars had accumulated —
        // 200 five-minute bars is two sessions, and after a restart the counter
        // starts at zero. Same shape as the dead-tape bug that froze bollinger's
        // time stop: risk management must never queue behind an indicator that
        // only the entry needs.
        const double pos = ctx.position(sym_).qty;
        if (pos > 0.0) {
            ++bars_held_;
            if (exit_id_ != 0) return;  // an exit is already in flight
            // The MA exit is the discretionary leg, so it is the one that pays
            // for itself: it may only fire once the close is min_gain_cps
            // cents/share above OUR fill. entry_px_ 0 means the position was
            // adopted at session start with no fill of ours behind it — no basis
            // to measure against, so fall back to the bare MA test rather than
            // hold something we cannot reason about (bollinger_reversion treats
            // an adopted position the same way).
            const bool gain_ok =
                entry_px_ <= 0.0 || bar.close >= entry_px_ + min_gain_cps_ / 100.0;
            const bool ma_ok = n >= static_cast<size_t>(exit_ma_) &&
                               bar.close > exit_sum_ / exit_ma_;
            // The time stop is a RISK exit and is deliberately NOT gated on
            // profit — the trade it exists for is precisely the one that never
            // gets there.
            const bool timed_out = bars_held_ >= time_stop_;
            if ((ma_ok && gain_ok) || timed_out) {
                exit_id_ = ctx.submit_order({sym_, Side::Sell, OrdType::Market, {},
                                             pos, 0.0, 0.0, 0.0, 0.0});
                if (exit_id_)
                    ctx.log(0, ma_ok && gain_ok ? "exit: close above exit MA"
                                                : "exit: time stop");
            }
            return;
        }
        bars_held_ = 0;

        if (n < static_cast<size_t>(trend_ma_) || rsi_n_ < rsi_len_) return;

        const double denom = avg_gain_ + avg_loss_;
        const double rsi = denom > 1e-12 ? 100.0 * avg_gain_ / denom : 50.0;

        if (entry_id_ != 0 || exit_id_ != 0) return;  // an order is in flight
        // Entry window — intraday bars only. A daily bar is stamped at the
        // session date, so a time-of-day window would reject every entry and
        // silently turn a daily backtest into a no-trade run.
        if (intraday()) {
            const double hod = hour_of_day_local(bar.ts_ns);
            if (hod < enter_from_h_ || hod >= enter_until_h_) return;
        }
        if (bar.close > trend_sum_ / trend_ma_ && rsi <= buy_below_) {
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
                std::snprintf(buf, sizeof(buf), "entry %.0f @ RSI %.1f", qty, rsi);
                ctx.log(0, buf);
            }
        }
    }

    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}

    void on_fill(IStrategyContext& ctx, const Fill& f) noexcept override {
        // The engine applies the fill BEFORE calling us, so this is the position
        // as it stands AFTER this print — which is what says whether the order
        // behind it is finished.
        const Position p = ctx.position(f.symbol_id);
        if (f.order_id == entry_id_) {
            entry_id_ = 0;
            bars_held_ = 0;
        } else if (f.order_id == exit_id_ && p.qty <= 0.0) {
            // Clear the in-flight guard only once the exit has actually CLOSED
            // the position. A market order arrives in pieces (SNXX filled 526
            // shares in five prints, 2026-08-06) and every print lands here, so
            // clearing on the first one re-opens the exit branch on the next bar
            // while the rest of the same order is still working at the broker.
            // `time_stop` is what makes that fatal: bars_held_ only ever grows,
            // so unlike the old `close > SMA(exit_ma)` test the condition never
            // goes false again, and the next bar submits a SECOND market sell
            // for the residual. Both fill and the account is left SHORT — a
            // state on_bar's `pos > 0.0` branch cannot see, let alone exit, and
            // one neither risk_ok nor clamp_to_notional blocks, because they
            // score the position they can see and not the sell still working.
            // on_order_end below still clears the id when the order DIES without
            // completing, which is the case that legitimately needs a resubmit.
            exit_id_ = 0;
        }
        // Read the basis back off the portfolio instead of trusting f.price:
        // avg_price is the average across every print of the entry, which would
        // otherwise leave the profit gate measuring against whichever print
        // landed last. It also keeps the basis alive through a PARTIAL exit;
        // dropping it there would read as "adopted" and quietly ungate the
        // shares still open. Flat means no basis, which is the adopted path and
        // is correct — there is nothing left to measure.
        entry_px_ = p.qty > 0.0 ? p.avg_price : 0.0;
    }

    // Without this, one rejected order leaves entry_id_ set and the in-flight
    // guard blocks every later entry for the rest of the session.
    void on_order_end(IStrategyContext&, const OrderEnd& e) noexcept override {
        if (e.order_id == entry_id_) entry_id_ = 0;
        else if (e.order_id == exit_id_) exit_id_ = 0;
    }

    void on_stop(IStrategyContext& ctx) noexcept override {
        ctx.log(1, "RSI2 stopped");
    }

    void destroy() noexcept override { delete this; }

private:
    // Bars shorter than a day carry a meaningful time of day; daily+ bars don't.
    bool intraday() const noexcept { return bar_sec_ > 0 && bar_sec_ < 23 * 3600; }

    int rsi_len_ = 2, exit_ma_ = 5, trend_ma_ = 200, time_stop_ = 24;
    double buy_below_ = 10, min_gain_cps_ = 3, budget_pct_ = 100, max_qty_ = 5000;
    double enter_from_h_ = 9.5, enter_until_h_ = 16;
    int bar_sec_ = 0;
    int64_t prev_ts_ = 0;

    uint32_t sym_ = 0;
    std::vector<double> closes_;
    double exit_sum_ = 0.0, trend_sum_ = 0.0;
    double prev_close_ = 0.0, avg_gain_ = 0.0, avg_loss_ = 0.0;
    int rsi_n_ = 0;
    uint64_t entry_id_ = 0, exit_id_ = 0;
    double entry_px_ = 0.0;   // our average fill on the open position (0 = none/adopted)
    int bars_held_ = 0;
};

TT_STRATEGY(Rsi2PullbackStrategy, "RSI-2 Pullback", kParams)
