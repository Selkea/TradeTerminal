#include "engine/engine.h"

#include "engine/broker.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>

namespace tt {

// True only on the live-session thread. Backtests and the live loop share one
// log queue, so every line is stamped with its origin here — the UI cannot
// otherwise tell them apart, and guessing from global state ("is a tournament
// running?") misfiles live output for most of the trading day.
static thread_local bool tt_on_live_thread = false;

namespace {
int64_t median_gap_ns(const std::vector<Bar>& bars) {
    if (bars.size() < 2) return 60'000'000'000;  // default 1 minute
    std::vector<int64_t> gaps;
    gaps.reserve(bars.size() - 1);
    for (size_t i = 1; i < bars.size(); ++i)
        gaps.push_back(bars[i].ts_ns - bars[i - 1].ts_ns);
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    return gaps[gaps.size() / 2];
}

// Shared by backtest and replay: fill in the derived statistics.
void finish_stats(BacktestResult& res, Portfolio& pf, LatencyHistogram& lat,
                  int64_t dt_ns, uint64_t events,
                  std::chrono::steady_clock::time_point wall_start) {
    using namespace std::chrono;
    res.final_equity = pf.equity();
    res.total_return = res.initial_cash != 0.0
                           ? res.final_equity / res.initial_cash - 1.0 : 0.0;
    double peak = -1e300, max_dd = 0.0;
    for (double v : res.eq_val) {
        peak = std::max(peak, v);
        if (peak > 0.0) max_dd = std::max(max_dd, (peak - v) / peak);
    }
    res.max_drawdown = max_dd;

    if (res.eq_val.size() > 2) {
        double sum = 0, sum2 = 0;
        size_t n = 0;
        for (size_t i = 1; i < res.eq_val.size(); ++i) {
            if (res.eq_val[i - 1] <= 0.0) continue;
            const double r = res.eq_val[i] / res.eq_val[i - 1] - 1.0;
            sum += r;
            sum2 += r * r;
            ++n;
        }
        if (n > 1) {
            const double mean = sum / static_cast<double>(n);
            const double var = sum2 / static_cast<double>(n) - mean * mean;
            const double sd = var > 0.0 ? std::sqrt(var) : 0.0;
            // Bars per year: daily bars => 252; intraday => 6.5h sessions.
            const double dt_sec = static_cast<double>(dt_ns) / 1e9;
            const double bars_per_year =
                dt_sec >= 0.9 * 86400.0 ? 252.0 : 252.0 * (23400.0 / dt_sec);
            if (sd > 0.0) res.sharpe = mean / sd * std::sqrt(bars_per_year);
        }
    }

    res.trades = static_cast<int>(res.fills.size());
    res.wins = pf.wins();
    res.losses = pf.losses();
    const int closed = res.wins + res.losses;
    res.win_rate = closed > 0 ? static_cast<double>(res.wins) / closed : 0.0;

    res.lat_p50 = lat.percentile_ns(0.50);
    res.lat_p99 = lat.percentile_ns(0.99);
    res.lat_max = lat.max_ns();
    res.lat_count = lat.count();
    res.events = events;
    res.duration_ms =
        duration_cast<microseconds>(steady_clock::now() - wall_start).count() / 1000.0;
}
} // namespace

// IStrategyContext implementation — lives on the engine thread's stack for
// the duration of one run (backtest or live).
class EngineCtx final : public IStrategyContext {
public:
    using NowFn = std::function<int64_t()>;
    // id != 0: the order was placed. id == 0: it was REFUSED, and `why` names
    // the cause (never RejectCause::None, never an empty message - see
    // engine/reject.h). `record` is false only for the refusals that repeat by
    // design and would flood a 200-entry blotter; they are still counted.
    using OrderHook = std::function<void(const OrderRequest&, uint64_t id,
                                         const RejectReason& why, bool record)>;

    EngineCtx(Engine& eng, const std::map<std::string, double>& params,
              const std::vector<std::string>& symbols, NowFn now, ExecSim& exec,
              Portfolio& pf, LatencyHistogram& lat, const RiskLimits* risk = nullptr,
              OrderHook on_order = {}, IBrokerAdapter* broker = nullptr,
              const std::vector<RiskLimits>* symbol_risk = nullptr,
              const std::vector<std::map<std::string, double>>* symbol_params = nullptr,
              const std::vector<uint8_t>* symbol_hold_only = nullptr)
        : eng_(eng), params_(params), now_(std::move(now)), exec_(exec), pf_(pf),
          lat_(lat), risk_(risk), symbol_risk_(symbol_risk),
          symbol_params_(symbol_params), symbol_hold_only_(symbol_hold_only),
          on_order_(std::move(on_order)), broker_(broker) {
        symbols_ = symbols;
    }

    // EVERY `return 0` below names a cause. That is the whole point of this
    // function's shape since 0.23.0: on 2026-08-13 the operator opened /diag,
    // saw two rejected orders with reject_code 0 and reject_msg "", and could
    // not distinguish a local risk gate from a buying-power refusal from
    // something IBKR said. Four of the seven refusal points here recorded a bare
    // code-0 rejection and three recorded nothing at all - not even a log line.
    uint64_t submit_order(const OrderRequest& in) noexcept override {
        // Replaying seed history to warm indicators: the strategy is seeing old
        // bars, so any signal it raises is historical. Warm it, place nothing.
        // Counted but not recorded: a warmup replays hundreds of bars and a
        // blotter row each would evict every real order in the session.
        if (warming_) return refuse(in, RejectCause::WarmupReplay, /*record=*/false);
        if (in.qty <= 0.0) return refuse(in, RejectCause::InvalidQty);
        if (in.symbol_id == 0 || in.symbol_id > symbols_.size())
            return refuse(in, RejectCause::SymbolNotInSession);
        // Pre-screen what ExecSim would otherwise refuse with a bare 0 - a
        // priceless limit/stop is a malformed request, and naming it here is the
        // difference between "sim_refused" and a cause an operator can fix.
        if (in.type == OrdType::Limit && in.limit_price <= 0.0)
            return refuse(in, RejectCause::MissingLimitPrice);
        if (in.type == OrdType::Stop && in.stop_price <= 0.0)
            return refuse(in, RejectCause::MissingStopPrice);
        // A quarantined symbol places nothing. run_live gates on_tick/on_bar
        // dispatch already, but on_fill is still delivered and could re-enter
        // through here - submit_order used to have no strat_halted knowledge at
        // all, which the dispatch-gate comment in run_live called out as the
        // remaining hole. Manual orders and the halt machinery's own flatten
        // bypass this function entirely, so closing is never blocked.
        if (!manual_ && symbol_halted(in.symbol_id))
            return refuse(in, RejectCause::StrategyHalted);
        // THE ENTRY GATE (see LiveConfig::entry_gate). Asked FIRST among the
        // sizing gates and asked of the wall clock, not of the bar: the bar a
        // strategy is reacting to opened one bar-length ago, which is exactly
        // how a 15:55 bar produced a 16:00:17 order into a shut exchange.
        // Position-increasing strategy orders only.
        if (entry_gate_ && !manual_ && in.outside_rth == 0 && increases_position(in) &&
            !entry_gate_(now_()))
            return refuse(in, RejectCause::SessionClosed);
        // HOLD-ONLY (see LiveConfig::symbol_hold_only). Same test as the entry
        // gate and for the same reason - reducing and exiting must always be
        // allowed, or carrying the symbol would trap the position it exists to
        // let out. Not conditional on `manual_`: an operator order on a
        // hold-only tab is far more likely to be a mistake than an intention.
        if (increases_position(in) && hold_only(in.symbol_id))
            return refuse(in, RejectCause::HoldOnlySymbol);
        if (const RejectCause c = risk_ok(in); c != RejectCause::None)
            return refuse(in, c);
        // Down-size a position-increasing order to the notional cap (see
        // clamp_to_notional). Reduce/close orders come back unchanged; an order
        // that can't add a single share (already at the cap), or one with no
        // price to size against at all, comes back as a cause.
        OrderRequest r = in;
        if (const RejectCause c = clamp_to_notional(r); c != RejectCause::None)
            return refuse(in, c);   // `in`: report the qty the strategy ASKED for
        // Hold-until-profitable: in "hold — don't halt" mode a strategy may not
        // close a position at a loss, so the position is held until it can exit
        // in profit. Counted but not recorded (a strategy retries its exit every
        // bar, and one row per retry would flood the blotter); the refusals
        // table carries the full count. Manual orders + the kill switch use
        // other paths.
        if (hold_blocks_loss(r))
            return refuse(r, RejectCause::HoldBlocksLoss, /*record=*/false);
        // Buying power (simulator only — a real broker enforces its own
        // margin): a risk-INCREASING order must be coverable at ~1x. Buys need
        // cash for the full cost; a new/larger short's notional must fit within
        // equity. Reducing or closing always passes, so exits are never blocked.
        if (!broker_ && increases_position(r)) {
            const double pos = pf_.position(r.symbol_id).qty;
            const double dir = r.side == Side::Buy ? 1.0 : -1.0;
            const double after = pos + dir * r.qty;
            const double px = price_for(r);
            bool ok = true;
            if (px > 0.0) {
                if (r.side == Side::Buy) ok = r.qty * px <= pf_.cash();
                else if (after < 0.0) ok = -after * px <= pf_.equity();
            }
            if (!ok) return refuse(r, RejectCause::BuyingPower);
        }
        const uint64_t id = broker_ ? broker_->submit(r, now_()) : exec_.submit(r, now_());
        if (!id) {
            // The venue refused it synchronously. Ask WHY rather than recording
            // the bare 0 that used to land in the blotter: TwsBroker alone has
            // four distinct reasons for this (unknown symbol, read-only account,
            // order path not connected, command ring full) and they call for
            // four different responses.
            RejectReason why = broker_ ? broker_->last_submit_reject()
                                       : RejectReason{RejectCause::SimRefused, 0,
                                                      reject_cause_text(RejectCause::SimRefused)};
            if (why.empty()) why = unexplained_broker_reject();
            if (why.message.empty()) why.message = reject_cause_text(why.cause);
            if (on_order_) on_order_(r, 0, why, true);
            return 0;
        }
        if (cur_event_tsc_)
            lat_.record(tsc::to_ns(rdtsc() - cur_event_tsc_));  // tick -> order
        if (on_order_) on_order_(r, id, RejectReason{}, true);
        return id;
    }
    bool cancel_order(uint64_t order_id) noexcept override {
        if (warming_) return false;   // nothing was placed during the replay
        return broker_ ? broker_->cancel(order_id) : exec_.cancel(order_id);
    }
    Position position(uint32_t symbol_id) const noexcept override {
        return pf_.position(symbol_id);
    }
    double cash() const noexcept override { return pf_.cash(); }
    // The sizing base (see IStrategyContext::budget). This is the same number
    // clamp_to_notional() enforces, handed to the strategy up front so its
    // percentage knobs mean something and its brackets are sized off the qty it
    // will actually get. kCashReserve leaves room for fees/slippage so a 100%
    // allocation can't trip the buying-power check on the fill.
    double budget(uint32_t symbol_id) const noexcept override {
        constexpr double kCashReserve = 0.95;
        const double spendable = pf_.cash() * kCashReserve;
        if (spendable <= 0.0) return 0.0;
        const RiskLimits* rl = resolve_risk(symbol_id);
        const double cap = rl ? rl->max_position_notional : 0.0;
        return cap > 0.0 ? std::min(cap, spendable) : spendable;
    }
    int64_t now_ns() const noexcept override { return now_(); }

    uint32_t symbol_id(const char* symbol) noexcept override {
        for (size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == symbol) return static_cast<uint32_t>(i + 1);
        symbols_.emplace_back(symbol);
        return static_cast<uint32_t>(symbols_.size());
    }
    double param(const char* name, double fallback) const noexcept override {
        // Per-symbol strategies each get their own params, keyed by the symbol
        // whose callback is currently running.
        if (symbol_params_ && cur_symbol_ >= 1 && cur_symbol_ <= symbol_params_->size()) {
            const auto& m = (*symbol_params_)[cur_symbol_ - 1];
            auto it = m.find(name);
            return it != m.end() ? it->second : fallback;
        }
        auto it = params_.find(name);
        return it != params_.end() ? it->second : fallback;
    }
    void log(int level, const char* msg) noexcept override {
        // A warmup replay pushes hundreds of historical bars through on_bar;
        // letting each one log would bury the session in thousands of lines.
        if (warming_) return;
        static constexpr const char* kLevels[] = {"debug", "info", "warn", "error"};
        const int l = level < 0 ? 0 : (level > 3 ? 3 : level);
        // Name the symbol whose callback is running. A bare "entry 5000 @ z=-1.50"
        // is unattributable once more than one symbol is live.
        std::string sym;
        if (cur_symbol_ >= 1 && cur_symbol_ <= symbols_.size())
            sym = symbols_[cur_symbol_ - 1] + ": ";
        eng_.push_log(std::string("[strategy ") + kLevels[l] + "] " + sym + msg);
    }

    void set_current_event_tsc(int64_t tsc) { cur_event_tsc_ = tsc; }
    // The symbol whose strategy callback is running now (per-symbol params).
    void set_current_symbol(uint32_t symbol_id) { cur_symbol_ = symbol_id; }
    // While true, submit_order() is a no-op (see LiveConfig::symbol_warmup).
    void set_warming(bool w) { warming_ = w; }
    // The operator pressed a button. Manual orders bypass the entry gate and the
    // halt quarantine — a human who has decided to trade is not the failure mode
    // either exists to prevent, and silently refusing them would be worse than
    // the 16:05 order that started all this.
    void set_manual(bool m) { manual_ = m; }
    // Wall-clock entry gate (LiveConfig::entry_gate); empty = disarmed.
    void set_entry_gate(std::function<bool(int64_t)> g) { entry_gate_ = std::move(g); }
    bool entry_gate_armed() const { return static_cast<bool>(entry_gate_); }
    bool entry_gate_open() const { return !entry_gate_ || entry_gate_(now_()); }
    // Views onto run_live's halt state (both declared after this object).
    void set_halt_view(const std::vector<char>* per_symbol) {
        strat_halted_ = per_symbol;
    }
    void set_last_price(uint32_t symbol_id, double p) {
        if (symbol_id == 0) return;
        if (last_price_.size() < symbol_id) last_price_.resize(symbol_id, 0.0);
        last_price_[symbol_id - 1] = p;
    }

private:
    // The ONE refusal exit. Builds the {cause, code, sentence} triple, hands it
    // to the recorder and returns 0. No path in submit_order may return 0
    // without going through here — that is what makes "a rejection with no
    // reason" unrepresentable rather than merely discouraged.
    uint64_t refuse(const OrderRequest& r, RejectCause cause,
                    bool record = true) noexcept {
        if (on_order_)
            on_order_(r, 0, RejectReason{cause, 0, reject_cause_text(cause)}, record);
        return 0;
    }

    // Does this order OPEN or ADD to a position (as opposed to reducing or
    // closing one)? The single test behind three different policies: the
    // notional clamp, the buying-power check and the entry gate all apply to
    // increases only, because refusing an exit is how positions go naked.
    bool increases_position(const OrderRequest& r) const noexcept {
        const double pos = pf_.position(r.symbol_id).qty;
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        return std::abs(pos + dir * r.qty) > std::abs(pos);
    }

    bool symbol_halted(uint32_t symbol_id) const noexcept {
        return strat_halted_ && symbol_id >= 1 && symbol_id <= strat_halted_->size() &&
               (*strat_halted_)[symbol_id - 1];
    }

    // Per-symbol limits when provided, else the session-level fallback.
    const RiskLimits* resolve_risk(uint32_t symbol_id) const noexcept {
        if (symbol_risk_ && symbol_id >= 1 && symbol_id <= symbol_risk_->size())
            return &(*symbol_risk_)[symbol_id - 1];
        return risk_;
    }

    // Best price to value an order at: its own limit/stop, else the last trade.
    double price_for(const OrderRequest& r) const noexcept {
        double px = r.type == OrdType::Limit  ? r.limit_price
                    : r.type == OrdType::Stop ? r.stop_price
                                              : 0.0;
        if (px <= 0.0 && r.symbol_id >= 1 && r.symbol_id <= last_price_.size())
            px = last_price_[r.symbol_id - 1];
        return px;
    }

    // Enforce max_position_notional by shrinking (never rejecting) a position-
    // INCREASING order so the resulting position's dollar size fits the cap.
    // Strategies size their protective bracket off the actual fill qty and their
    // exits off the live position, so a smaller fill stays fully protected.
    //
    // Returns None when the order may proceed (r.qty possibly reduced), else the
    // cause it must be refused with. Reduce/close orders always return None:
    // this function must never be the reason a position cannot be closed.
    RejectCause clamp_to_notional(OrderRequest& r) noexcept {
        const double pos = pf_.position(r.symbol_id).qty;
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        const double after = pos + dir * r.qty;
        if (std::abs(after) <= std::abs(pos))
            return RejectCause::None;   // reducing/closing: leave it
        // AN UNPRICEABLE ORDER IS NOT SIZEABLE, so it is not sendable. This
        // used to `return` and let the order through at full size with the
        // dollar cap silently unapplied — an order sized against a price nobody
        // has, which is the failure the 2026-08-13 review went looking for
        // (correctly, as it turned out: it was not what happened that day, but
        // the hole was real). Refusing it up front means such an order is never
        // built rather than being caught downstream by an equity halt after it
        // has already traded. Increases only: an exit with no quote still goes.
        const double px = price_for(r);
        if (px <= 0.0) return RejectCause::PriceUnavailable;
        const RiskLimits* rl = resolve_risk(r.symbol_id);
        if (!rl || rl->max_position_notional <= 0.0) return RejectCause::None;
        const double max_abs = rl->max_position_notional / px;   // shares the cap allows
        if (std::abs(after) <= max_abs) return RejectCause::None;   // within budget
        const double allowed_add = std::floor(max_abs - std::abs(pos));
        if (allowed_add <= 0.0) return RejectCause::NotionalCap;   // not one share fits
        r.qty = allowed_add;
        return RejectCause::None;
    }

    // "hold — don't halt" mode (RiskLimits::disable_auto_halt): a strategy order
    // that would CLOSE/REDUCE a position at a loss is held instead — protective
    // stops, signal exits and EOD flattens don't fire while underwater, so the
    // position rides until it can exit in profit (bounded by the notional cap and
    // the stale-feed halt, both still armed). Opening/adding and profitable exits
    // pass; this is per strategy order, so it covers every strategy uniformly.
    bool hold_blocks_loss(const OrderRequest& r) noexcept {
        const RiskLimits* rl = resolve_risk(r.symbol_id);
        if (!rl || !rl->disable_auto_halt) return false;
        const auto pos = pf_.position(r.symbol_id);
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        if (pos.qty == 0.0 || dir * pos.qty >= 0.0) return false;  // opening/adding: allow
        const double px = price_for(r);
        if (px <= 0.0 || pos.avg_price <= 0.0) return false;       // no reference: allow
        const double closed = std::min(r.qty, std::abs(pos.qty));
        const double pnl = pos.qty > 0.0 ? (px - pos.avg_price) * closed   // long exit
                                         : (pos.avg_price - px) * closed;  // short exit
        return pnl < 0.0;   // would realize a loss → hold
    }

    // None = passes. Returns the specific gate that stopped it: the three used
    // to share one bare code-0 rejection, so /diag could not tell an oversized
    // order from a fat-fingered limit price.
    RejectCause risk_ok(const OrderRequest& r) noexcept {
        if (!risk_ && !symbol_risk_) return RejectCause::None;
        const RiskLimits* rl = resolve_risk(r.symbol_id);
        if (!rl) return RejectCause::None;
        if (r.qty > rl->max_order_qty) return RejectCause::MaxOrderQty;
        const double pos = pf_.position(r.symbol_id).qty;
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        if (std::abs(pos + dir * r.qty) > rl->max_position_qty)
            return RejectCause::MaxPositionQty;
        const double last =
            r.symbol_id > 0 && r.symbol_id <= last_price_.size() ? last_price_[r.symbol_id - 1] : 0.0;
        if (r.type == OrdType::Limit && last > 0.0 &&
            std::abs(r.limit_price - last) / last > rl->price_band_pct)
            return RejectCause::PriceBand;
        return RejectCause::None;
    }

    Engine& eng_;
    const std::map<std::string, double>& params_;
    NowFn now_;
    ExecSim& exec_;
    Portfolio& pf_;
    LatencyHistogram& lat_;
    const RiskLimits* risk_;
    const std::vector<RiskLimits>* symbol_risk_ = nullptr;
    const std::vector<std::map<std::string, double>>* symbol_params_ = nullptr;
    const std::vector<uint8_t>* symbol_hold_only_ = nullptr;
    // May this symbol open or add? Absent vector = every symbol may.
    bool hold_only(uint32_t symbol_id) const noexcept {
        return symbol_hold_only_ && symbol_id >= 1 &&
               symbol_id <= symbol_hold_only_->size() &&
               (*symbol_hold_only_)[symbol_id - 1] != 0;
    }
    uint32_t cur_symbol_ = 0;
    OrderHook on_order_;
    bool warming_ = false;
    bool manual_ = false;
    std::function<bool(int64_t)> entry_gate_;
    const std::vector<char>* strat_halted_ = nullptr;
    IBrokerAdapter* broker_;
    std::vector<std::string> symbols_;
    int64_t cur_event_tsc_ = 0;
    std::vector<double> last_price_;
};

Engine::Engine() { tsc::calibrate(); }

Engine::~Engine() {
    stop_live();
    if (engine_thread_.joinable()) engine_thread_.join();
}

void Engine::push_log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.emplace_back(std::move(line), tt_on_live_thread);
    while (logs_.size() > 1000) logs_.pop_front();
}

bool Engine::pop_log(std::string& out) {
    bool ignored = false;
    return pop_log(out, ignored);
}

bool Engine::pop_log(std::string& out, bool& from_live) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front().first);
    from_live = logs_.front().second;
    logs_.pop_front();
    return true;
}

void Engine::push_fill(const FillRecord& f) {
    std::lock_guard lock(fill_mu_);
    fill_feed_.push_back(f);
    while (fill_feed_.size() > 1000) fill_feed_.pop_front();
}

bool Engine::pop_fill(FillRecord& out) {
    std::lock_guard lock(fill_mu_);
    if (fill_feed_.empty()) return false;
    out = fill_feed_.front();
    fill_feed_.pop_front();
    return true;
}

bool Engine::take_result(BacktestResult& out) {
    if (!has_result_.exchange(false)) return false;
    std::lock_guard lock(result_mu_);
    out = std::move(result_);
    return true;
}

bool Engine::start_backtest(BacktestConfig cfg, IStrategy* strategy) {
    if (running_.exchange(true)) return false;
    if (engine_thread_.joinable()) engine_thread_.join();  // previous run
    engine_thread_ = std::thread(
        [this, cfg = std::move(cfg), strategy]() mutable { run(std::move(cfg), strategy); });
    return true;
}

void Engine::run(BacktestConfig cfg, IStrategy* strategy) {
    using namespace std::chrono;
    const auto wall_start = steady_clock::now();

    BacktestClock clock;
    ExecSim exec(cfg.exec);
    Portfolio pf(cfg.initial_cash);
    LatencyHistogram lat;
    // THE ORDER-LEVEL RISK (BacktestConfig::risk), which this call site defaulted
    // to nullptr for the engine's whole life — taking ctx.budget()'s cap with it,
    // so every strategy sized off the entire account. cfg outlives ctx (it is a
    // by-value member of this frame), so the pointer stays good for the run.
    const RiskLimits* bt_risk = cfg.risk ? &*cfg.risk : nullptr;
    EngineCtx ctx(*this, cfg.params, std::vector<std::string>{cfg.symbol},
                  [&clock] { return clock.now_ns(); }, exec, pf, lat, bt_risk);
    // THE ENTRY GATE, on the replay's own clock (BacktestConfig::entry_cutoff_h).
    // The live gate is asked of the WALL clock because a strategy reacts to a bar
    // that opened one bar-length ago; here the clock IS that later instant —
    // BacktestClock is set to ev.ts_event_ns, the bar's COMPLETION — so asking it
    // reproduces the live question exactly rather than approximating it.
    if (cfg.entry_cutoff_h > 0.0) {
        const double cutoff = cfg.entry_cutoff_h;
        ctx.set_entry_gate([cutoff](int64_t now_ns) {
            return hour_of_day_local(now_ns) < cutoff;
        });
    }

    BacktestResult res;
    res.symbol = cfg.symbol;
    res.initial_cash = cfg.initial_cash;
    res.eq_ts.reserve(cfg.bars.size());
    res.eq_val.reserve(cfg.bars.size());

    const int64_t dt = median_gap_ns(cfg.bars);
    constexpr uint32_t kSymbolId = 1;

    strategy->on_init(ctx);

    // Local ring: a backtest may run concurrently with a live session (the
    // Optimizer does this), and md_ring_ belongs to the live loop — two
    // consumers on one SPSC ring steal each other's events.
    const auto bt_ring = std::make_unique<MdRing>();

    // Feed thread: replays the series at full speed. Backpressure by
    // spinning — the backtest must be lossless.
    std::thread feed([&bt_ring, &cfg, dt] {
        auto push = [&bt_ring](EngineEvent& ev) {
            ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
            while (!bt_ring->try_push(ev)) { /* spin: consumer is draining */ }
        };
        for (const Bar& b : cfg.bars) {
            if (cfg.synth_ticks) {
                const double prices[4] = {b.open, b.high, b.low, b.close};
                for (int i = 0; i < 4; ++i) {
                    EngineEvent ev{};
                    ev.type = static_cast<uint16_t>(EvType::Tick);
                    ev.symbol_id = kSymbolId;
                    ev.ts_event_ns = b.ts_ns + dt * i / 4;
                    ev.u.tick.price = prices[i];
                    ev.u.tick.size = 0.0;
                    ev.u.tick.bid = prices[i];
                    ev.u.tick.ask = prices[i];
                    push(ev);
                }
            }
            EngineEvent ev{};
            ev.type = static_cast<uint16_t>(EvType::Bar);
            ev.symbol_id = kSymbolId;
            ev.ts_event_ns = b.ts_ns + dt;  // bar completes at open + interval
            ev.u.bar.open = b.open;
            ev.u.bar.high = b.high;
            ev.u.bar.low = b.low;
            ev.u.bar.close = b.close;
            ev.u.bar.volume = b.volume;
            push(ev);
        }
        EngineEvent end{};
        end.type = static_cast<uint16_t>(EvType::End);
        push(end);
    });

    // THE DAY BOUNDARY (BacktestConfig::eod_flatten_h). Off unless the caller
    // asks — see the field for the full 2026-08-14 post-mortem. When on, it
    // reproduces run_live's 15:57 EOD backstop inside the replay: cancel every
    // resting order, market-close the position, and let the strategy hear both.
    //
    // Without it the optimizer scores holds that production force-flattens out
    // of existence, which is how bollinger_reversion's MANDATORY time stop —
    // the only exit a losing position of its has — got fitted to 233 bars on a
    // 300 s series, three times longer than a trading day.
    //
    // EDGE-triggered on the crossing, exactly like the live backstop: a level
    // test would re-flatten on every remaining bar of the evening and turn a
    // series with after-hours bars into a stream of closes.
    double eod_prev_h = -1.0;
    auto eod_flatten = [&](double price, int64_t now_ns) {
        if (!(cfg.eod_flatten_h > 0.0)) return;
        const double hod = hour_of_day_local(now_ns);
        const double prev = eod_prev_h;
        eod_prev_h = hod;
        if (prev < 0.0 || prev >= cfg.eod_flatten_h || hod < cfg.eod_flatten_h) return;
        // Resting orders die first, and the strategy is TOLD. Skipping the
        // callback would leave an in-flight guard latched (every strategy here
        // keeps one — see bollinger_reversion's entry_id_/exit_id_) and wedge
        // the symbol for the rest of the backtest, which would score as "this
        // fit stops trading after day one" rather than as a flat position.
        for (uint64_t id : exec.cancel_all())
            strategy->on_order_end(ctx, OrderEnd{id, kSymbolId,
                                                 OrderEndReason::Cancelled, {},
                                                 now_ns, 0, {}});
        const double pos = pf.position(kSymbolId).qty;
        if (pos == 0.0) return;   // flat already: the normal path
        // Through ExecSim rather than a synthesized Fill, so the close pays the
        // same slippage and the same IBKR commission every other fill in the run
        // pays. A free liquidation would flatter exactly the fits this exists to
        // penalise. Priced at this bar, so the deadline handed to on_price has
        // to clear the modeled wire latency the submit just added.
        const OrderRequest r{kSymbolId, pos > 0 ? Side::Sell : Side::Buy,
                             OrdType::Market, {}, std::abs(pos), 0.0, 0.0, 0.0, 0.0};
        exec.submit(r, now_ns);
        std::vector<Fill> eod_fills;
        exec.on_price(kSymbolId, price,
                      now_ns + cfg.exec.latency_ns + cfg.exec.latency_jitter_ns,
                      eod_fills);
        for (const Fill& f : eod_fills) {
            pf.apply(f);
            res.fills.push_back(TradeRow{f.ts_ns, static_cast<uint8_t>(f.side),
                                         f.qty, f.price, f.fee});
            strategy->on_fill(ctx, f);
        }
    };

    // Engine loop: single consumer, deterministic order.
    std::vector<Fill> fills;
    fills.reserve(16);
    EngineEvent ev;
    uint64_t events = 0;
    for (;;) {
        if (!bt_ring->try_pop(ev)) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
            continue;
        }
        ++events;
        if (ev.type == static_cast<uint16_t>(EvType::End)) break;

        clock.set(ev.ts_event_ns);
        ctx.set_current_event_tsc(ev.ts_ingest_tsc);

        const double price = ev.type == static_cast<uint16_t>(EvType::Tick)
                                 ? ev.u.tick.price
                                 : ev.u.bar.close;
        ctx.set_last_price(ev.symbol_id, price);   // price band + buying power

        fills.clear();
        exec.on_price(ev.symbol_id, price, clock.now_ns(), fills);
        for (const Fill& f : fills) {
            pf.apply(f);
            res.fills.push_back(TradeRow{f.ts_ns, static_cast<uint8_t>(f.side),
                                         f.qty, f.price, f.fee});
            strategy->on_fill(ctx, f);
        }
        // Orders the sim dropped (an OCO sibling killed by the fill above, or
        // anything on_fill cancelled). Backtests are where the optimizer grades
        // every candidate, so a wedge that only fails live would never show up
        // in a score — the callback has to fire here too.
        for (const SimCancel& c : exec.take_cancels())
            strategy->on_order_end(ctx, OrderEnd{c.order_id, c.symbol_id,
                                                 OrderEndReason::Cancelled, {},
                                                 clock.now_ns(), 0, {}});
        pf.mark(ev.symbol_id, price);

        if (ev.type == static_cast<uint16_t>(EvType::Tick)) {
            Tick t{ev.ts_event_ns, ev.u.tick.price, ev.u.tick.size,
                   ev.u.tick.bid, ev.u.tick.ask};
            strategy->on_tick(ctx, ev.symbol_id, t);
        } else if (ev.type == static_cast<uint16_t>(EvType::Bar)) {
            Bar b{ev.ts_event_ns - dt, ev.u.bar.open, ev.u.bar.high,
                  ev.u.bar.low, ev.u.bar.close, ev.u.bar.volume};
            strategy->on_bar(ctx, ev.symbol_id, b);
            // AFTER the strategy, so a strategy that closes its own position on
            // this bar is never second-guessed, and the equity point below
            // already reflects the liquidation when it does not.
            eod_flatten(price, ev.ts_event_ns);
            res.eq_ts.push_back(static_cast<double>(ev.ts_event_ns) / 1e9);
            res.eq_val.push_back(pf.equity());
        }
    }
    feed.join();
    strategy->on_stop(ctx);

    finish_stats(res, pf, lat, dt, events, wall_start);

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "backtest %s: %llu events in %.1f ms, return %+.2f%%, %d fills",
                  cfg.symbol.c_str(), static_cast<unsigned long long>(events),
                  res.duration_ms, res.total_return * 100.0, res.trades);
    {
        std::lock_guard lock(result_mu_);
        result_ = std::move(res);
    }
    has_result_ = true;
    running_ = false;
    push_log(buf);
}

bool Engine::start_replay(ReplayConfig cfg, IStrategy* strategy) {
    if (running_.exchange(true)) return false;
    if (engine_thread_.joinable()) engine_thread_.join();  // previous run
    engine_thread_ = std::thread(
        [this, cfg = std::move(cfg), strategy]() mutable { run_replay(std::move(cfg), strategy); });
    return true;
}

// Deterministic re-run of a captured session: events are fed inline on the
// engine thread (no ring, no feed thread — nothing to race), the clock is
// the recorded event time, and fills come from ExecSim.
void Engine::run_replay(ReplayConfig cfg, IStrategy* strategy) {
    using namespace std::chrono;
    const auto wall_start = steady_clock::now();

    BacktestClock clock;
    ExecSim exec(cfg.exec);
    Portfolio pf(cfg.initial_cash);
    LatencyHistogram lat;
    EngineCtx ctx(*this, cfg.params, cfg.log.symbols,
                  [&clock] { return clock.now_ns(); }, exec, pf, lat);

    BacktestResult res;
    res.symbol = cfg.name;
    res.initial_cash = cfg.initial_cash;

    const size_t n_sym = cfg.log.symbols.size();
    // Replay at an overridden bar size when asked, else the recorded one.
    const int bar_seconds =
        cfg.bar_seconds_override > 0 ? cfg.bar_seconds_override : cfg.log.bar_seconds;
    const int64_t bar_ns = static_cast<int64_t>(bar_seconds) * 1'000'000'000;

    // Same tick->bar aggregation as the live loop, so bar strategies replay
    // the way they ran.
    struct BarAgg { Bar cur{}; bool open = false; };
    std::vector<BarAgg> bar_agg(n_sym);
    auto roll_bar = [&](uint32_t symbol_id, int64_t ts, double px) {
        if (symbol_id == 0 || symbol_id > n_sym) return;
        BarAgg& agg = bar_agg[symbol_id - 1];
        if (agg.open && ts >= agg.cur.ts_ns + bar_ns) {
            strategy->on_bar(ctx, symbol_id, agg.cur);
            res.eq_ts.push_back(static_cast<double>(ts) / 1e9);
            res.eq_val.push_back(pf.equity());
            agg.open = false;
        }
        if (!agg.open) {
            agg.open = true;
            agg.cur = Bar{ts / bar_ns * bar_ns, px, px, px, px, 0.0};
            return;
        }
        agg.cur.high = std::max(agg.cur.high, px);
        agg.cur.low = std::min(agg.cur.low, px);
        agg.cur.close = px;
    };

    strategy->on_init(ctx);

    std::vector<Fill> fills;
    fills.reserve(16);
    uint64_t events = 0;
    for (const EngineEvent& ev : cfg.log.events) {
        if (ev.symbol_id == 0 || ev.symbol_id > n_sym) continue;
        if (ev.type == static_cast<uint16_t>(EvType::Bar)) {
            // Captured backfill bar: replay it to the strategy exactly as
            // the live loop delivered it (no fills, no marks).
            ++events;
            clock.set(ev.ts_event_ns);
            const Bar b{ev.ts_event_ns, ev.u.bar.open, ev.u.bar.high,
                        ev.u.bar.low, ev.u.bar.close, ev.u.bar.volume};
            // run_backtest sets the last price before every dispatch; this
            // branch did not, so a strategy ordering off a captured backfill bar
            // was priced at 0 — the price band and the buying-power check were
            // both blind, and since 0.23.0 an unpriceable increase is refused
            // outright (RejectCause::PriceUnavailable) rather than sent unsized.
            ctx.set_last_price(ev.symbol_id, b.close);
            strategy->on_bar(ctx, ev.symbol_id, b);
            continue;
        }
        if (ev.type != static_cast<uint16_t>(EvType::Tick)) continue;
        ++events;
        clock.set(ev.ts_event_ns);
        // Captured ingest timestamps are meaningless at replay time; leaving
        // the event tsc at 0 keeps the latency histogram honest (empty).
        ctx.set_current_event_tsc(0);
        const double price = ev.u.tick.price;
        ctx.set_last_price(ev.symbol_id, price);

        fills.clear();
        exec.on_price(ev.symbol_id, price, clock.now_ns(), fills);
        for (const Fill& f : fills) {
            pf.apply(f);
            res.fills.push_back(TradeRow{f.ts_ns, static_cast<uint8_t>(f.side),
                                         f.qty, f.price, f.fee});
            strategy->on_fill(ctx, f);
        }
        for (const SimCancel& c : exec.take_cancels())   // see run_backtest
            strategy->on_order_end(ctx, OrderEnd{c.order_id, c.symbol_id,
                                                 OrderEndReason::Cancelled, {},
                                                 clock.now_ns(), 0, {}});
        pf.mark(ev.symbol_id, price);

        Tick t{ev.ts_event_ns, price, ev.u.tick.size, ev.u.tick.bid, ev.u.tick.ask};
        strategy->on_tick(ctx, ev.symbol_id, t);
        roll_bar(ev.symbol_id, ev.ts_event_ns, price);
    }
    strategy->on_stop(ctx);

    finish_stats(res, pf, lat, bar_ns, events, wall_start);

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "replay %s: %llu ticks in %.1f ms, return %+.2f%%, %d fills",
                  cfg.name.c_str(), static_cast<unsigned long long>(events),
                  res.duration_ms, res.total_return * 100.0, res.trades);
    {
        std::lock_guard lock(result_mu_);
        result_ = std::move(res);
    }
    has_result_ = true;
    running_ = false;
    push_log(buf);
}

// ------------------------------------------------------------------- live

std::vector<std::string> Engine::live_symbols() const {
    std::lock_guard lock(snap_mu_);
    return live_symbol_table_;
}

LiveSnapshot Engine::live_snapshot() const {
    LiveSnapshot s;
    std::shared_ptr<const std::vector<OrderRecord>> ord;
    {
        std::lock_guard lock(snap_mu_);
        s = snap_;          // scalars + small symbols vector; orders are empty
        ord = snap_orders_;
    }
    if (ord) s.orders = *ord;   // immutable: safe to copy outside the lock
    s.dropped_ticks = dropped_ticks_.load(std::memory_order_relaxed);
    return s;
}

void Engine::push_live_tick(const std::string& symbol, int64_t ts_ms, double price,
                            double day_volume) {
    if (!live_running_.load(std::memory_order_relaxed)) return;
    uint32_t symbol_id = 0;
    {
        std::lock_guard lock(snap_mu_);
        for (size_t i = 0; i < live_symbol_table_.size(); ++i)
            if (live_symbol_table_[i] == symbol) { symbol_id = static_cast<uint32_t>(i + 1); break; }
    }
    if (symbol_id == 0) return;  // not part of this live session

    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::Tick);
    ev.symbol_id = symbol_id;
    ev.ts_event_ns = ts_ms * 1'000'000;
    ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
    ev.u.tick.price = price;
    ev.u.tick.size = day_volume;
    ev.u.tick.bid = price;
    ev.u.tick.ask = price;
    if (!md_ring_->try_push(ev))
        dropped_ticks_.fetch_add(1, std::memory_order_relaxed);  // stale ticks are worthless
}

void Engine::request_cancel(uint64_t order_id) {
    cmd_ring_->try_push(LiveCmd{LiveCmd::Cancel, 0, order_id, 0, 0});
}

void Engine::submit_manual(uint32_t symbol_id, bool buy, double qty,
                           double take_profit, double stop_loss,
                           double limit_price, bool outside_rth) {
    LiveCmd c{};
    c.type = LiveCmd::Manual;
    c.buy = buy ? 1 : 0;
    c.qty = qty;
    c.symbol_id = symbol_id;
    c.take_profit = take_profit;
    c.stop_loss = stop_loss;
    c.limit_price = limit_price;
    c.outside_rth = outside_rth ? 1 : 0;
    cmd_ring_->try_push(c);
}

void Engine::kill_switch() {
    cmd_ring_->try_push(LiveCmd{LiveCmd::Kill, 0, 0, 0, 0});
}

void Engine::queue_swap(PendingSwap s) {
    if (!live_running_.load(std::memory_order_relaxed) || s.symbol_id == 0) return;
    std::lock_guard lock(swap_mu_);
    // Latest swap per symbol wins.
    for (auto& p : pending_swaps_)
        if (p.symbol_id == s.symbol_id) {
            p = std::move(s);
            has_swaps_.store(true, std::memory_order_release);
            return;
        }
    pending_swaps_.push_back(std::move(s));
    has_swaps_.store(true, std::memory_order_release);
}

void Engine::update_symbol_params(uint32_t symbol_id, std::map<std::string, double> params,
                                  std::vector<Bar> warmup) {
    queue_swap(PendingSwap{symbol_id, nullptr, std::move(params), std::move(warmup)});
}

void Engine::reseed_symbol(uint32_t symbol_id, std::vector<Bar> warmup) {
    if (warmup.empty()) return;
    PendingSwap s{symbol_id, nullptr, {}, std::move(warmup), /*keep_params=*/true};
    queue_swap(std::move(s));
}

void Engine::swap_symbol_strategy(uint32_t symbol_id, IStrategy* strategy,
                                  std::map<std::string, double> params,
                                  std::vector<Bar> warmup) {
    if (!strategy) return;
    queue_swap(PendingSwap{symbol_id, strategy, std::move(params), std::move(warmup)});
}

void Engine::stop_live(bool keep_broker_orders) {
    if (live_running_.load(std::memory_order_relaxed))
        // Stop reuses the `buy` byte to carry keep_broker_orders (see run_live's
        // Stop handler + the cancel-on-stop gate).
        while (!cmd_ring_->try_push(
            LiveCmd{LiveCmd::Stop, static_cast<uint8_t>(keep_broker_orders ? 1 : 0),
                    0, 0, 0}))
            Sleep(1);
    if (live_thread_.joinable()) live_thread_.join();
}

bool Engine::start_live(LiveConfig cfg, std::vector<IStrategy*> strategies) {
    if (running_.load(std::memory_order_relaxed)) return false;
    if (live_running_.exchange(true)) return false;
    if (live_thread_.joinable()) live_thread_.join();

    // Fresh rings for the new session.
    EngineEvent ev;
    while (md_ring_->try_pop(ev)) {}
    while (feed_ring_->try_pop(ev)) {}
    LiveCmd c;
    while (cmd_ring_->try_pop(c)) {}
    dropped_ticks_ = 0;
    {
        std::lock_guard lock(snap_mu_);
        snap_ = LiveSnapshot{};
        snap_orders_.reset();
        snap_.running = true;
        snap_.cash = cfg.initial_cash;
        snap_.equity = cfg.initial_cash;
        for (size_t i = 0; i < cfg.symbols.size(); ++i) {
            SymbolState st;
            st.symbol = cfg.symbols[i];
            st.position.symbol_id = static_cast<uint32_t>(i + 1);
            snap_.symbols.push_back(std::move(st));
        }
        live_symbol_table_ = cfg.symbols;
    }
    live_thread_ = std::thread(
        [this, cfg = std::move(cfg), strategies = std::move(strategies)]() mutable {
            run_live(std::move(cfg), std::move(strategies));
        });
    return true;
}

void Engine::run_live(LiveConfig cfg, std::vector<IStrategy*> strategies) {
    // Stamp everything this thread logs as live (see tt_on_live_thread).
    tt_on_live_thread = true;
    // The trading thread outranks everything else in this process.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cfg.pin_core >= 0 && cfg.pin_core < 64) {
        if (SetThreadAffinityMask(GetCurrentThread(), 1ull << cfg.pin_core))
            push_log("live: engine thread pinned to core " + std::to_string(cfg.pin_core));
        else
            push_log("live: could not pin engine thread to core " +
                     std::to_string(cfg.pin_core));
    }
    RealTimeClock rt;
    ExecSim exec(cfg.exec);
    Portfolio pf(cfg.initial_cash);
    LatencyHistogram lat;
    bool halted = false;

    std::vector<OrderRecord> orders;   // engine-thread master copy
    bool orders_dirty = true;          // snapshot copies the vector only on change
    bool params_dirty = true;          // same, for the per-symbol param maps
    bool next_is_manual = false;
    auto symbol_name = [&](uint32_t symbol_id) -> std::string {
        return symbol_id > 0 && symbol_id <= cfg.symbols.size()
                   ? cfg.symbols[symbol_id - 1] : std::string();
    };
    auto record_order = [&](const OrderRequest& r, uint64_t id,
                            const RejectReason& why = {}) {
        OrderRecord rec;
        rec.id = id;
        rec.ts_ns = rt.now_ns();
        rec.symbol_id = r.symbol_id;
        rec.symbol = symbol_name(r.symbol_id);
        rec.side = static_cast<uint8_t>(r.side);
        rec.type = static_cast<uint8_t>(r.type);
        rec.status = id ? OrderStatus::Working : OrderStatus::Rejected;
        rec.qty = r.qty;
        rec.limit_price = r.limit_price;
        rec.manual = next_is_manual;
        if (rec.status == OrderStatus::Rejected) {
            // THE INVARIANT (engine/reject.h): a rejected record always says
            // why. Callers that hand a submit straight to the broker/exec pass
            // no reason at all, so the generic one stands in rather than the
            // blank that reached /diag on 2026-08-13.
            rec.reject_cause =
                why.cause != RejectCause::None ? why.cause : RejectCause::BrokerRefused;
            rec.reject_code = why.code;
            rec.reject_msg =
                why.message.empty() ? reject_cause_text(rec.reject_cause) : why.message;
        }
        orders.push_back(rec);
        if (orders.size() > 200) {
            // Evict the oldest FINISHED row, not simply the oldest. A WORKING
            // row is the only thing that can name the symbol, side and size of
            // an event that arrives for that id later, and a bracket's resting
            // legs live for days on a session that spans days (this one has).
            // Dropping one turned a broker rejection into an unattributable
            // "order #N rejected" with no refusal row and no page. Still hard
            // bounded: if all 200 are working the oldest goes anyway.
            auto victim = std::find_if(orders.begin(), orders.end(),
                                       [](const OrderRecord& o) {
                                           return o.status != OrderStatus::Working;
                                       });
            orders.erase(victim == orders.end() ? orders.begin() : victim);
        }
        orders_dirty = true;
    };

    // ---- refusal bookkeeping (engine/reject.h) ------------------------------
    // One row per (symbol, cause) for the whole session, plus the alerting
    // policy. Bounded by symbols x causes, so it cannot flood the way a blotter
    // row per refusal would.
    std::vector<RefusalStat> refusals;
    bool refusals_dirty = true;
    uint64_t entry_gate_blocked = 0;

    // ALERTING POLICY, argued here because it is the difference between a page
    // that gets read and a channel that gets muted:
    //
    //  * A single refusal of an ENTRY is Info (webhook, no beep). Most refusals
    //    are the system working — the notional clamp finding a position already
    //    at its cap, the entry gate declining to buy into a shut exchange. A
    //    beep for each would train the operator to ignore the beep, which is
    //    precisely what the "half-open" data-feed spam did before it was
    //    de-escalated (see alert_rules.h).
    //  * The SAME (symbol, cause) three times is a Warning (beep). One refusal
    //    is a decision; a repeat is a strategy stuck in a loop it cannot leave,
    //    and it means that symbol is silently not trading. Three rather than two
    //    because one bar can legitimately produce an entry and its bracket leg
    //    and have both hit the same gate (kRefusalRepeatAlertAt).
    //  * ANY refusal of an EXIT — an order that would reduce or close a live
    //    position — is Critical on the FIRST occurrence, regardless of cause. An
    //    exit that cannot be placed is how a position ends up with nothing that
    //    will ever close it, which is this system's worst failure class and
    //    already owns the Critical tier ("PROTECTIVE STOP REJECTED"). There is
    //    no benign version of it, so there is no repeat threshold.
    //
    // hold_blocks_loss is the one deliberate exception: it refuses exits by
    // design (that is what hold-until-profitable IS), so it is counted and
    // logged once per symbol, never paged. Its row in `refusals` is how an
    // operator sees a position being ridden.
    // Is this order an EXIT — would it strictly SHRINK a position that is open
    // right now? Deliberately not "the opposite of an increase": a zero-qty or
    // malformed order increases nothing, and calling that an exit would page
    // Critical for a strategy bug that risks no position at all. Evaluated
    // against the same portfolio the refusal was decided on, before any fill can
    // move it. This one bool decides Critical vs Info (see note_refusal).
    auto order_reduces_position = [&](const OrderRequest& r) {
        const double pos = pf.position(r.symbol_id).qty;
        if (pos == 0.0) return false;   // nothing open to close
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        return std::abs(pos + dir * r.qty) < std::abs(pos);
    };
    // known_exit: the CALLER knows this order would have reduced a position and
    // the request cannot say so itself. A broker rejection for an id the blotter
    // no longer holds (a bracket's take-profit leg, whose id is minted inside
    // the adapter and never returned from submit(); a resting leg whose row aged
    // out) arrives with no side and no qty, so order_reduces_position would call
    // the most dangerous refusal in the system an entry.
    auto note_refusal = [&](const OrderRequest& r, const RejectReason& why, bool record,
                            bool known_exit = false) {
        const bool is_exit = known_exit || order_reduces_position(r);
        RefusalStat* row = nullptr;
        for (RefusalStat& s : refusals)
            if (s.symbol_id == r.symbol_id && s.cause == why.cause) { row = &s; break; }
        const bool first = row == nullptr;
        if (first) {
            RefusalStat fresh;
            fresh.symbol_id = r.symbol_id;
            fresh.symbol = symbol_name(r.symbol_id);
            fresh.cause = why.cause;
            fresh.code = why.code;
            fresh.message = why.message;
            fresh.first_ts_ns = rt.now_ns();
            refusals.push_back(std::move(fresh));
            row = &refusals.back();
        }
        // "The first time this pair was refused" and "the first time an EXIT of
        // this pair was refused" are DIFFERENT questions, and conflating them is
        // what made a refused exit silent. The (symbol, cause) key is shared
        // with entry refusals, so an earlier harmless entry refusal — the entry
        // gate declining a buy at 16:05, say — created the row and every later
        // refused exit on that symbol for that cause then said nothing at all.
        const bool first_exit = is_exit && !row->exit_order;
        ++row->count;
        row->last_ts_ns = rt.now_ns();
        row->exit_order = row->exit_order || is_exit;
        if (is_exit) ++row->exit_count;
        refusals_dirty = true;
        if (why.cause == RejectCause::SessionClosed) ++entry_gate_blocked;
        // A blotter row for the first of each pair, and for the first EXIT of
        // each pair (see RefusalStat): reject_count is the field the operator
        // watches, and a refused exit that never moved it is how this whole
        // investigation started. Still at most two rows per pair, so a strategy
        // retrying into the same gate cannot evict the session's real orders.
        if (record && (first || first_exit)) record_order(r, 0, why);

        // A warmup replay is not a refused order — the strategy is being shown
        // history and nothing was ever meant to reach the market. Counted so a
        // set_warming(true) that never got cleared is visible, but never logged
        // and never paged.
        if (why.cause == RejectCause::WarmupReplay) return;
        const std::string sym = symbol_name(r.symbol_id);
        // qty 0 = rebuilt from an event, not from the blotter: the side and size
        // are genuinely unknown and saying "buy 0" would invent them.
        const std::string what =
            std::string(" [") + reject_cause_slug(why.cause) + "] " + sym + " " +
            (r.qty > 0.0 ? std::string(r.side == Side::Buy ? "buy " : "sell ") +
                               std::to_string(static_cast<long long>(r.qty))
                         : std::string("order (size unknown - see the broker line "
                                       "below for the id)")) +
            ": " + why.message;
        // Hold-until-profitable is the one refusal that is SUPPOSED to repeat:
        // the strategy re-offers its exit every bar and hold mode declines it
        // every bar until the position is green. It says so once (Info) and then
        // stays quiet — escalating it to Warning at the third bar would page for
        // every held position, all day, which is how a channel gets muted. Its
        // row in `refusals` is where an operator sees it is still happening.
        // Checked before the exit branch: it refuses exits BY DESIGN.
        if (why.cause == RejectCause::HoldBlocksLoss) {
            if (first) push_log(std::string("live: ") + kOrderRefusedTag + what);
            return;
        }
        if (is_exit) {
            // First one always pages; after that, on the backoff schedule in
            // reject.h. Never "once, then silence" — the position it would have
            // closed is still open for as long as this keeps happening.
            const int64_t now = rt.now_ns();
            if (row->exit_pages == 0 || now - row->exit_paged_ns >= row->exit_repage_ns) {
                push_log(std::string("live: ") + kExitOrderRefusedTag + what +
                         (row->exit_count > 1
                              ? " (x" + std::to_string(row->exit_count) + ")"
                              : ""));
                ++row->exit_pages;
                row->exit_paged_ns = now;
                row->exit_repage_ns = std::min(
                    row->exit_repage_ns * kExitRefusalRepageGrowth, kExitRefusalRepageMaxNs);
            }
            return;
        }
        if (first) push_log(std::string("live: ") + kOrderRefusedTag + what);
        else if (row->count >= row->warn_at) {
            // >=, not ==: see kRefusalRepeatAlertAt. The counter advances in the
            // branches above too, so an == test can have its one value consumed
            // by a refusal that never reaches this line.
            push_log(std::string("live: ") + kOrderRefusedRepeatTag + what + " (x" +
                     std::to_string(row->count) + ")");
            row->warn_at = row->count * 10;   // says so again at 30, 300, ...
        }
    };
    auto on_order = [&](const OrderRequest& r, uint64_t id, const RejectReason& why,
                        bool record) {
        if (id) { record_order(r, id); return; }
        note_refusal(r, why, record);
    };
    // The three paths that bypass submit_order entirely and hand a flatten
    // straight to the broker/simulator: the kill switch, the EOD backstop and
    // the watchdog/protective halt. All three close positions, so a 0 from them
    // is the worst refusal this app can produce — and it used to land in the
    // blotter as a bare Rejected row with no cause and no page at all. Routed
    // through the same bookkeeping so it rates Critical like any other refused
    // exit.
    auto record_submit = [&](const OrderRequest& r, uint64_t id) {
        if (id) { record_order(r, id); return; }
        RejectReason why = cfg.broker
                               ? cfg.broker->last_submit_reject()
                               : RejectReason{RejectCause::SimRefused, 0,
                                              reject_cause_text(RejectCause::SimRefused)};
        if (why.empty()) why = unexplained_broker_reject();
        if (why.message.empty()) why.message = reject_cause_text(why.cause);
        note_refusal(r, why, /*record=*/true);
    };

    IBrokerAdapter* const broker = cfg.broker;
    // Hot-swap target: one param slot per symbol (ctx reads through the
    // pointer, so in-place updates are visible immediately after a re-init).
    // Stale swaps from a previous session are dropped.
    cfg.symbol_params.resize(cfg.symbols.size());
    // Sized unconditionally so a later hot-swap always has a slot to drop its
    // fresh seed bars into, even when the session started with none.
    cfg.symbol_warmup.resize(cfg.symbols.size());
    {
        std::lock_guard lock(swap_mu_);
        pending_swaps_.clear();
        has_swaps_.store(false, std::memory_order_relaxed);
    }
    EngineCtx ctx(*this, cfg.params, cfg.symbols, [&rt] { return rt.now_ns(); },
                  exec, pf, lat, &cfg.risk, on_order, broker, &cfg.symbol_risk,
                  &cfg.symbol_params, &cfg.symbol_hold_only);
    ctx.set_entry_gate(cfg.entry_gate);
    // The gate cannot fail silently. An app that forgot to install one gets one
    // line at session start saying so, and /diag carries entry_gate.armed for
    // the rest of the session — an unarmed live session is the 2026-08-13
    // configuration, where a strategy with enter_until_h 24 was free to buy at
    // 04:05 and have IBKR fill it at the open five hours later.
    push_log(cfg.entry_gate
                 ? "live: entry gate ARMED - strategies may only OPEN or ADD to a "
                   "position while the exchange is open (exits are never gated)"
                 : "live: entry gate OFF - strategies may enter at ANY hour, "
                   "including pre-market and after the close");
    // Route an event's symbol to the strategy instance that owns it.
    auto strat_for = [&](uint32_t sid) -> IStrategy* {
        return sid >= 1 && sid <= strategies.size() ? strategies[sid - 1] : nullptr;
    };

    TickLogWriter capture;
    if (!cfg.capture_path.empty()) {
        if (capture.open(cfg.capture_path, cfg.symbols, cfg.bar_seconds))
            push_log("live: recording ticks to " + cfg.capture_path);
        else
            push_log("live: cannot open capture file " + cfg.capture_path);
    }

    const size_t n_sym = cfg.symbols.size();
    uint64_t ticks = 0;
    std::vector<double> last_price(n_sym, 0.0);
    int64_t last_ts_ms = 0;

    // Automated-halt anchors, declared here so publish() can report the current
    // headroom; check_risk (below) reads/updates them, and broker reconciliation
    // re-anchors both to the reconciled equity.
    double risk_base_eq = pf.equity();   // equity at session start
    double risk_high_eq = risk_base_eq;  // running session equity high

    // ---- broker reconciliation (a restarted session adopting live positions) ----
    // A reconciling broker (reconciles()==true) replays its existing positions,
    // resting orders, and cash on connect and ends with a ReconcileEnd event.
    // Until then `reconciling` gates ALL strategy dispatch, so nothing trades
    // against a not-yet-known position. Any symbol that comes back holding a
    // position is then kept paused via adopt_hold — its adopted broker-side
    // stop/TP protects and exits it — until it goes flat, at which point its
    // strategy re-inits and resumes. The 10s failsafe lifts the gate if the
    // broker never signals (e.g. connect failed) so a stall can't wedge trading.
    // No-op for non-reconciling brokers (paper/sim/web): the flags never engage.
    bool reconciling = broker && broker->reconciles();
    // The failsafe that lifts the reconcile gate is armed only once the broker
    // reports ready (handshake done, reconciliation about to replay) — NOT at
    // session start. Anchoring the 10s to session start meant a slow cold-gateway
    // connect (the connect watchdog allows up to 30s) could blow the deadline
    // before reconciliation even began: the gate would lift, the strategy would
    // trade a flat/placeholder book, then a late PosSnap would clobber the
    // position it had built. 0 = not yet armed. Until the broker is ready there
    // is nothing to trade against anyway (orders would fail).
    int64_t reconcile_deadline_ns = 0;
    std::vector<char> adopt_hold(n_sym, 0);

    // Percentiles are recomputed only when a new sample landed (orders are
    // rare next to ticks) — publish() itself stays flat.
    uint64_t lat_seen = 0;
    int64_t lat_p50 = 0, lat_p99 = 0;
    auto publish = [&] {
        if (lat.count() != lat_seen) {
            lat_seen = lat.count();
            lat_p50 = lat.percentile_ns(0.50);
            lat_p99 = lat.percentile_ns(0.99);
        }
        // String-heavy orders copy happens before taking the lock, and only
        // when orders actually changed; under the lock it's a pointer swap.
        std::shared_ptr<const std::vector<OrderRecord>> fresh;
        if (orders_dirty) {
            fresh = std::make_shared<const std::vector<OrderRecord>>(orders);
            orders_dirty = false;
        }
        std::lock_guard lock(snap_mu_);
        snap_.running = true;
        snap_.halted = halted;
        if (refusals_dirty) {
            snap_.refusals = refusals;
            refusals_dirty = false;
        }
        // Recomputed every publish, not cached: "is the market open" is a
        // question about NOW, and a cached answer is how a gate reads open all
        // evening. Cheap - the app's predicate is a localtime + a calendar test.
        snap_.entry_gate_armed = ctx.entry_gate_armed();
        snap_.entry_gate_open = ctx.entry_gate_open();
        snap_.entry_gate_blocked = entry_gate_blocked;
        snap_.reconciled = !reconciling;
        snap_.cash = pf.cash();
        snap_.equity = pf.equity();
        snap_.risk.daily_loss = risk_base_eq - snap_.equity;
        snap_.risk.daily_loss_limit = cfg.risk.daily_max_loss;
        snap_.risk.drawdown_pct =
            risk_high_eq > 0 ? (risk_high_eq - snap_.equity) / risk_high_eq : 0.0;
        snap_.risk.drawdown_limit_pct = cfg.risk.max_drawdown_pct;
        snap_.risk.stale_feed_sec = cfg.risk.stale_feed_sec;
        for (size_t i = 0; i < n_sym; ++i) {
            snap_.symbols[i].last_price = last_price[i];
            snap_.symbols[i].position = pf.position(static_cast<uint32_t>(i + 1));
            snap_.symbols[i].fees = pf.fees(static_cast<uint32_t>(i + 1));
            snap_.symbols[i].adopted = adopt_hold[i] != 0;
        }
        // Params change only at session start and on a swap, so copy the maps
        // then rather than on every publish.
        if (params_dirty) {
            for (size_t i = 0; i < n_sym && i < cfg.symbol_params.size(); ++i)
                snap_.symbols[i].params = cfg.symbol_params[i];
            params_dirty = false;
        }
        if (fresh) snap_orders_ = std::move(fresh);
        snap_.ticks = ticks;
        snap_.last_tick_ts_ms = last_ts_ms;
        snap_.lat_p50 = lat_p50;
        snap_.lat_p99 = lat_p99;
        snap_.lat_max = lat.max_ns();
        snap_.lat_count = lat_seen;
    };

    // The snapshot copy (mutex + orders vector) is the heaviest work in the
    // loop, and the UI samples it at ~60 Hz — publishing per tick is pure
    // waste under a fast feed. Tick updates coalesce to one publish per
    // ~16 ms; fills, commands, and halts publish immediately.
    constexpr int64_t kPublishInterval = 16'000'000;
    int64_t last_pub_ns = 0;
    bool snap_dirty = false;
    auto publish_throttled = [&](int64_t now) {
        if (now - last_pub_ns >= kPublishInterval) {
            publish();
            last_pub_ns = now;
            snap_dirty = false;
        } else {
            snap_dirty = true;
        }
    };

    // Per-symbol strategy halt flag (set by the watchdog / protective-reject
    // safety net below). Declared here so the fill path can refuse to dispatch
    // on_fill to a halted symbol, same as on_tick/on_bar do.
    std::vector<char> strat_halted(n_sym, 0);
    // ...and so submit_order can refuse an order from a quarantined symbol with
    // a named cause instead of relying on dispatch gating alone (on_fill is
    // still delivered to a halted symbol's strategy).
    ctx.set_halt_view(&strat_halted);
    // Shared by the sim fill path and the broker event drain.
    auto apply_fill = [&](const Fill& f) {
        pf.apply(f);
        for (auto& o : orders)
            if (o.id == f.order_id) {
                o.status = OrderStatus::Filled;
                o.fill_price = f.price;
                o.fee = f.fee;
                orders_dirty = true;
            }
        // Lead with the SYMBOL: without it a fill line cannot be attributed at
        // all, and journal.db was the only place that carried it. The order id
        // stays so a fill still pairs with its "tws: order #N acked" line.
        const char* fsym = (f.symbol_id >= 1 && f.symbol_id <= cfg.symbols.size())
                               ? cfg.symbols[f.symbol_id - 1].c_str()
                               : "?";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "live: %s fill %s %.0f @ %.2f (order #%llu)",
                      fsym, f.side == Side::Buy ? "BUY" : "SELL", f.qty, f.price,
                      static_cast<unsigned long long>(f.order_id));
        push_log(buf);
        push_fill(FillRecord{f.ts_ns, f.order_id, f.symbol_id,
                             static_cast<uint8_t>(f.side), f.qty, f.price, f.fee});
        // Don't dispatch on_fill when the strategy shouldn't be running: a held
        // (adopted) symbol's fills come from broker-side resting orders the
        // paused strategy never placed (the drain's resume handles it going
        // flat); a watchdog/protective-halted symbol has been quarantined; and
        // during reconciliation no strategy is trading yet. Matches the
        // on_tick/on_bar dispatch gates so a halted strategy can't re-enter via
        // on_fill (submit_order has no strat_halted knowledge).
        const bool in_range = f.symbol_id >= 1 && f.symbol_id <= n_sym;
        const bool held = in_range && adopt_hold[f.symbol_id - 1];
        const bool sym_halted = in_range && strat_halted[f.symbol_id - 1];
        if (!halted && !held && !sym_halted && !reconciling)
            if (IStrategy* s = strat_for(f.symbol_id)) {
                ctx.set_current_symbol(f.symbol_id);
                s->on_fill(ctx, f);
            }
    };

    // An order left the book without completing. Strategies store in-flight
    // order ids and gate new entries on them ("an order is in flight, wait"),
    // and a dead id never appears in on_fill — so a death nobody reports wedges
    // that symbol until the next on_init, i.e. the whole session, silently.
    // Gated exactly like apply_fill: submit_order has no halt knowledge, so a
    // halted/held/reconciling strategy must not be woken here either.
    auto notify_order_end = [&](uint64_t id, uint32_t sid, OrderEndReason reason,
                                int32_t code) {
        if (id == 0) return;
        if (sid == 0)   // sim cancels carry the symbol; broker events may not
            for (const auto& o : orders)
                if (o.id == id) {
                    sid = o.symbol_id;
                    break;
                }
        if (sid < 1 || sid > n_sym) return;   // e.g. a sim bracket child
        if (halted || adopt_hold[sid - 1] || strat_halted[sid - 1] || reconciling)
            return;
        if (IStrategy* s = strat_for(sid)) {
            ctx.set_current_symbol(sid);
            s->on_order_end(ctx, OrderEnd{id, sid, reason, {}, rt.now_ns(), code, {}});
        }
    };

    // The simulator has no cancel event, so every order it drops (an explicit
    // cancel, or an OCO sibling going with it) surfaces only here. Marks the
    // order table too — without this a strategy-cancelled order shows Working
    // in the blotter forever.
    auto drain_sim_cancels = [&] {
        for (const SimCancel& c : exec.take_cancels()) {
            for (auto& o : orders)
                if (o.id == c.order_id && o.status == OrderStatus::Working) {
                    o.status = OrderStatus::Cancelled;
                    orders_dirty = true;
                }
            notify_order_end(c.order_id, c.symbol_id, OrderEndReason::Cancelled, 0);
        }
    };

    // Kill-switch behavior, shared by the manual command and automated risk
    // halts: cancel everything, flatten, halt the strategy.
    auto kill_all = [&](const std::string& why) {
        halted = true;
        if (broker) {
            broker->cancel_all();
            broker->flatten();
            push_log("live: " + why +
                     " — broker cancel-all + flatten requested, strategy halted");
            return;
        }
        for (uint64_t id : exec.cancel_all())
            for (auto& o : orders)
                if (o.id == id) {
                    o.status = OrderStatus::Cancelled;
                    orders_dirty = true;
                }
        for (size_t i = 0; i < n_sym; ++i) {
            const uint32_t sid = static_cast<uint32_t>(i + 1);
            const double pos = pf.position(sid).qty;
            if (pos == 0.0) continue;
            // Flatten bypasses risk checks — closing must always work.
            OrderRequest r{sid, pos > 0 ? Side::Sell : Side::Buy,
                           OrdType::Market, {}, std::abs(pos), 0.0, 0.0, 0.0, 0.0};
            next_is_manual = true;
            record_submit(r, exec.submit(r, rt.now_ns()));
            next_is_manual = false;
        }
        push_log("live: " + why + " — all orders cancelled, flattening, strategy halted");
    };

    // End-of-day backstop: nothing rides overnight unmanaged.
    //
    // Strategy-level EOD logic only ever covers the strategy that OPENED the
    // position. A lineup swap re-homes a symbol onto a DIFFERENT strategy and
    // pauses it via adopt_hold, so the position it inherits has nobody driving
    // it flat — and if no protective order was adopted with it, nothing will
    // ever close it. SNXX rode 8.35 -> 10.34 that way on 2026-08-06 while its
    // new strategy sat paused waiting for a fill that could not come.
    //
    // Engine-level, so it applies to every symbol regardless of which strategy
    // (if any) is running it. Deliberately LATER than the strategies' own EOD
    // (ORB flattens at 15:54) and the UI's scheduled stop (15:55): this only
    // fires when both of those already failed. Does NOT halt — a live session
    // can span days (this one has), and halting here would kill tomorrow.
    //
    // Hold mode (disable_auto_halt) opts out: that setting exists to ride a
    // position through rather than realise it, and this must not fight it.
    // 15:57 local. tt::kEodBackstopH in terminal/src/market_calendar.h mirrors
    // this: the entry gate stops OPENING positions at the same instant this
    // starts closing them, because this is edge-triggered and marks the day done
    // when it fires — anything opened after it has nothing left to close it.
    constexpr double kEodBackstopH = 15.95;
    int64_t eod_day = -1;
    double eod_prev_h = -1.0;   // last hour-of-day seen; -1 = first observation
    auto eod_backstop = [&] {
        if (cfg.risk.disable_auto_halt) return;   // ride it through, by design
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        const int64_t day = static_cast<int64_t>(tm.tm_year) * 400 + tm.tm_yday;
        const double hod = tm.tm_hour + tm.tm_min / 60.0;
        const double prev_h = eod_prev_h;
        eod_prev_h = hod;
        if (day == eod_day) return;   // already handled today
        // TODAY's cutoff, not a constant: 15:57 on a normal session, 12:57 on the
        // three 13:00 early closes, and 0 on a day the market never opens at all.
        // See LiveConfig::eod_flatten_h_for_day for what the constant cost.
        const double flat_h = cfg.eod_flatten_h_for_day
                                  ? cfg.eod_flatten_h_for_day(tm)
                                  : kEodBackstopH;
        // Nothing opens today, so nothing may be force-closed into it either. A
        // weekend session holding an adopted position used to be liquidated at
        // 15:57 on a Saturday, at market, against a shut exchange.
        if (flat_h <= 0.0) return;
        // EDGE-triggered: the session must have been running and watched the
        // clock cross the cutoff. A level trigger would flatten the instant the
        // engine came up at, say, 16:10 — turning a restart into a liquidation
        // of positions the user may well have meant to hold.
        if (prev_h < 0.0 || prev_h >= flat_h || hod < flat_h) return;

        bool any_pos = false;
        for (size_t i = 0; i < n_sym && !any_pos; ++i)
            any_pos = pf.position(static_cast<uint32_t>(i + 1)).qty != 0.0;
        eod_day = day;          // mark done either way: don't re-check all evening
        if (!any_pos) return;   // flat already — the normal path, stay quiet

        // The cutoff is in the message because it is no longer always 15:57, and
        // a log line that names the wrong hour is worse than one that names none:
        // terminal.log carries no date, so the hour is most of what a post-mortem
        // has to place the event by.
        char eod_msg[160];
        const int flat_hh = static_cast<int>(flat_h);
        const int flat_mm = static_cast<int>((flat_h - flat_hh) * 60.0 + 0.5);

        if (broker) {
            broker->cancel_all();
            broker->flatten();
            std::snprintf(eod_msg, sizeof(eod_msg),
                          "live: EOD BACKSTOP — open position(s) past %02d:%02d with "
                          "no strategy closing them; broker cancel-all + flatten "
                          "requested", flat_hh, flat_mm);
            push_log(eod_msg);
            return;
        }
        for (uint64_t id : exec.cancel_all())
            for (auto& o : orders)
                if (o.id == id) {
                    o.status = OrderStatus::Cancelled;
                    orders_dirty = true;
                }
        for (size_t i = 0; i < n_sym; ++i) {
            const uint32_t sid = static_cast<uint32_t>(i + 1);
            const double pos = pf.position(sid).qty;
            if (pos == 0.0) continue;
            OrderRequest r{sid, pos > 0 ? Side::Sell : Side::Buy,
                           OrdType::Market, {}, std::abs(pos), 0.0, 0.0, 0.0, 0.0};
            next_is_manual = true;   // closing must always work
            record_submit(r, exec.submit(r, rt.now_ns()));
            next_is_manual = false;
        }
        std::snprintf(eod_msg, sizeof(eod_msg),
                      "live: EOD BACKSTOP — open position(s) past %02d:%02d with no "
                      "strategy closing them; cancelled + flattening",
                      flat_hh, flat_mm);
        push_log(eod_msg);
    };

    // Automated halts: equity-based limits re-checked after every fill/tick.
    auto check_risk = [&] {
        if (halted) return;
        const double eq = pf.equity();
        risk_high_eq = std::max(risk_high_eq, eq);
        if (cfg.risk.daily_max_loss > 0 && risk_base_eq - eq >= cfg.risk.daily_max_loss)
            kill_all("RISK HALT (daily loss limit)");
        else if (cfg.risk.max_drawdown_pct > 0 && risk_high_eq > 0 &&
                 (risk_high_eq - eq) / risk_high_eq >= cfg.risk.max_drawdown_pct)
            kill_all("RISK HALT (session drawdown limit)");
    };
    int64_t last_md_ns = rt.now_ns();

    // True only for the first symbol pointing at a given instance, so shared
    // instances get on_init/on_stop exactly once.
    auto first_use = [&](size_t i) {
        for (size_t j = 0; j < i; ++j)
            if (strategies[j] == strategies[i]) return false;
        return true;
    };
    // Replay this symbol's seed bars through its (freshly initialised) strategy
    // so lookback indicators are warm immediately. In a live session the ONLY
    // source of on_bar is tick aggregation (roll_bar below), which yields at
    // most one bar per bar_seconds — so a 365-bar lookback on 5m bars needs
    // ~30 hours of continuous session to warm, and the daily lineup swap
    // re-inits every morning. Without this, such a strategy never signals.
    // Called directly rather than through the feed-event path: that path stamps
    // the tick counter, the capture file and the stale-feed watchdog before it
    // looks at the event type, and none of that should move for replayed history.
    auto replay_warmup = [&](uint32_t symbol_id) {
        const size_t i = symbol_id - 1;
        if (i >= cfg.symbol_warmup.size() || i >= strategies.size()) return;
        const std::vector<Bar>& seed = cfg.symbol_warmup[i];
        IStrategy* s = strategies[i];
        if (seed.empty() || !s) return;
        ctx.set_current_symbol(symbol_id);
        ctx.set_warming(true);
        for (const Bar& b : seed) s->on_bar(ctx, symbol_id, b);
        ctx.set_warming(false);
        push_log("live: " + cfg.symbols[i] + ": warmed on " +
                 std::to_string(seed.size()) + " seed bar(s)");
    };

    // Each symbol's strategy instance gets its own on_init.
    for (size_t i = 0; i < strategies.size(); ++i)
        if (strategies[i] && first_use(i)) {
            ctx.set_current_symbol(static_cast<uint32_t>(i + 1));
            strategies[i]->on_init(ctx);
            replay_warmup(static_cast<uint32_t>(i + 1));
        }
    std::string symbol_list;
    for (const std::string& s : cfg.symbols)
        symbol_list += (symbol_list.empty() ? "" : ",") + s;
    push_log(std::string("live: ") + (broker ? "BROKER trading " : "paper trading ") +
             symbol_list + " started (bar " + std::to_string(cfg.bar_seconds) + "s)" +
             (broker && !broker->ready() ? " — waiting for broker connection" : ""));

    // Per-symbol strategy watchdog: if a callback overruns the budget, halt that
    // symbol (stop dispatching to it) and flatten its position — one slow or
    // runaway strategy must not keep degrading the shared engine thread. Note:
    // this catches callbacks that *return* slowly; a true infinite loop can't be
    // unwound from this thread (nothing safely can). (strat_halted is declared
    // above apply_fill so the fill path can read it too.)
    const int64_t watchdog_ns = static_cast<int64_t>(cfg.watchdog_ms) * 1'000'000;
    // Flatten one symbol's position at market and stop dispatching to its
    // strategy. Shared by the watchdog and the protective-reject safety net.
    auto flatten_halt_symbol = [&](uint32_t sid, const std::string& why) {
        if (sid < 1 || sid > n_sym || strat_halted[sid - 1]) return;
        strat_halted[sid - 1] = 1;
        // Cancel this symbol's resting orders first. Once halted its strategy
        // can't manage its own manual OCO (on_tick/on_bar are gated), so an
        // orphaned exit leg (protective stop / take-profit) left resting could
        // fill AFTER the flatten and reverse the position into a naked,
        // unmanaged one. (kill_all does the same via broker/exec cancel_all.)
        for (auto& o : orders)
            if (o.symbol_id == sid && o.status == OrderStatus::Working) {
                if (broker) {
                    broker->cancel(o.id);   // async: settles on the cancel event
                } else if (exec.cancel(o.id)) {
                    o.status = OrderStatus::Cancelled;
                    orders_dirty = true;
                }
            }
        const double pos = pf.position(sid).qty;
        if (pos != 0.0) {
            OrderRequest r{sid, pos > 0 ? Side::Sell : Side::Buy, OrdType::Market,
                           {}, std::abs(pos), 0.0, 0.0, 0.0, 0.0};
            next_is_manual = true;   // bypass risk: closing must always work
            record_submit(r, broker ? broker->submit(r, rt.now_ns())
                                    : exec.submit(r, rt.now_ns()));
            next_is_manual = false;
        }
        push_log("live: " + why);
    };
    auto halt_symbol = [&](uint32_t sid, int64_t dur_ns) {
        if (sid < 1 || sid > n_sym || strat_halted[sid - 1]) return;
        char buf[176];
        std::snprintf(buf, sizeof buf,
                      "WATCHDOG halt — %s strategy callback ran %lld ms "
                      "(budget %d ms); halted + flattened",
                      cfg.symbols[sid - 1].c_str(),
                      static_cast<long long>(dur_ns / 1'000'000), cfg.watchdog_ms);
        flatten_halt_symbol(sid, buf);
    };
    // Time a strategy callback; halt the symbol if it overruns. No-op when off.
    auto guarded = [&](uint32_t sid, auto&& fn) {
        if (watchdog_ns <= 0) { fn(); return; }
        const uint64_t t0 = rdtsc();
        fn();
        const int64_t d = tsc::to_ns(static_cast<int64_t>(rdtsc() - t0));
        if (d > watchdog_ns) halt_symbol(sid, d);
    };

    // Apply queued hot-swaps for symbols that are FLAT: install the new params
    // (and instance, for a strategy swap), lift any watchdog halt, and re-init
    // so the strategy re-reads its params with clean state. Symbols holding a
    // position stay queued until they go flat. (Instances are per-symbol in
    // the app; a shared instance would be reset for all its symbols.)
    auto apply_swaps = [&] {
        std::vector<PendingSwap> ready;
        {
            std::lock_guard lock(swap_mu_);
            for (size_t i = 0; i < pending_swaps_.size();) {
                PendingSwap& s = pending_swaps_[i];
                if (s.symbol_id < 1 || s.symbol_id > n_sym) {
                    pending_swaps_.erase(pending_swaps_.begin() +
                                         static_cast<ptrdiff_t>(i));
                    continue;
                }
                if (pf.position(s.symbol_id).qty != 0.0) {   // wait for flat
                    ++i;
                    continue;
                }
                ready.push_back(std::move(s));
                pending_swaps_.erase(pending_swaps_.begin() + static_cast<ptrdiff_t>(i));
            }
            has_swaps_.store(!pending_swaps_.empty(), std::memory_order_release);
        }
        for (PendingSwap& s : ready) {
            const size_t i = s.symbol_id - 1;
            if (!s.keep_params) {
                cfg.symbol_params[i] = std::move(s.params);
                params_dirty = true;   // republish what is actually running
            }
            if (s.strategy) strategies[i] = s.strategy;
            if (!strategies[i]) continue;
            strat_halted[i] = 0;   // fresh start: watchdog halt lifted
            ctx.set_current_event_tsc(0);
            ctx.set_current_symbol(s.symbol_id);
            strategies[i]->on_init(ctx);
            // on_init wipes the strategy's bar history, so a params update would
            // otherwise reset warmup to zero every autopilot cycle. Take the
            // swap's fresh bars when it carried any, else re-use the session's.
            if (!s.warmup.empty() && i < cfg.symbol_warmup.size())
                cfg.symbol_warmup[i] = std::move(s.warmup);
            replay_warmup(s.symbol_id);
            push_log("live: " + cfg.symbols[i] +
                     (s.strategy      ? ": strategy swapped"
                      : s.keep_params ? ": re-seeded"
                                      : ": params updated") +
                     " (re-init while flat)");
        }
    };

    // Tick -> bar aggregation so bar-based strategies work on the live feed,
    // one aggregator per symbol, each at its own bar size (falls back to the
    // session default when no per-symbol size was given).
    std::vector<int64_t> bar_ns(n_sym);
    for (size_t i = 0; i < n_sym; ++i) {
        const int bs = (i < cfg.symbol_bar_seconds.size() && cfg.symbol_bar_seconds[i] > 0)
                           ? cfg.symbol_bar_seconds[i] : cfg.bar_seconds;
        bar_ns[i] = static_cast<int64_t>(bs) * 1'000'000'000;
    }
    struct BarAgg { Bar cur{}; bool open = false; };
    std::vector<BarAgg> bar_agg(n_sym);
    auto roll_bar = [&](uint32_t symbol_id, int64_t ts, double px) {
        if (symbol_id == 0 || symbol_id > n_sym) return;
        const int64_t bn = bar_ns[symbol_id - 1];
        BarAgg& agg = bar_agg[symbol_id - 1];
        if (agg.open && ts >= agg.cur.ts_ns + bn) {
            if (!halted && !strat_halted[symbol_id - 1] && !reconciling &&
                !adopt_hold[symbol_id - 1])
                if (IStrategy* s = strat_for(symbol_id)) {
                    ctx.set_current_symbol(symbol_id);
                    guarded(symbol_id, [&] { s->on_bar(ctx, symbol_id, agg.cur); });
                }
            agg.open = false;
        }
        if (!agg.open) {
            agg.open = true;
            agg.cur = Bar{ts / bn * bn, px, px, px, px, 0.0};
            return;
        }
        agg.cur.high = std::max(agg.cur.high, px);
        agg.cur.low = std::min(agg.cur.low, px);
        agg.cur.close = px;
    };

    std::vector<Fill> fills;
    fills.reserve(8);
    int idle = 0;
    bool stop = false;
    bool keep_broker_orders = false;   // set by a keep-positions Stop command
    while (!stop) {
        // Failsafe: arm the deadline when the broker first reports ready, then
        // lift the gate if reconciliation hasn't signalled completion within the
        // window (connect fine but the replay stalled) so a stall can't wedge
        // trading (degrades to no adoption). Anchored to ready, not session
        // start — see reconcile_deadline_ns above.
        if (reconciling && reconcile_deadline_ns == 0 && broker && broker->ready())
            reconcile_deadline_ns = rt.now_ns() + 10'000'000'000LL;
        if (reconciling && reconcile_deadline_ns != 0 &&
            rt.now_ns() > reconcile_deadline_ns) {
            reconciling = false;
            push_log("live: broker reconciliation timed out — trading normally");
        }
        // Hot-swaps: one relaxed load per iteration; the work happens only
        // when the autopilot actually queued something.
        if (has_swaps_.load(std::memory_order_acquire)) apply_swaps();

        LiveCmd c;
        bool had_cmd = false;
        while (cmd_ring_->try_pop(c)) {
            had_cmd = true;
            switch (c.type) {
            case LiveCmd::Stop:
                stop = true;
                keep_broker_orders = c.buy != 0;   // keep-positions restart
                break;
            case LiveCmd::Cancel:
                if (broker) {
                    // Async: status settles when the broker's cancel event lands.
                    broker->cancel(c.order_id);
                    push_log("live: cancel requested for order #" + std::to_string(c.order_id));
                } else if (exec.cancel(c.order_id)) {
                    for (auto& o : orders)
                        if (o.id == c.order_id) {
                            o.status = OrderStatus::Cancelled;
                            orders_dirty = true;
                        }
                    push_log("live: cancelled order #" + std::to_string(c.order_id));
                    drain_sim_cancels();   // and tell the strategy its id died
                }
                break;
            case LiveCmd::Kill:
                kill_all("KILL SWITCH");
                break;
            case LiveCmd::Manual: {
                const uint32_t sid = c.symbol_id;
                if (sid == 0 || sid > n_sym || last_price[sid - 1] <= 0.0) {
                    push_log("live: manual order rejected (no market data yet)");
                    break;
                }
                const bool lmt = c.limit_price > 0.0;
                OrderRequest r{sid, c.buy ? Side::Buy : Side::Sell,
                               lmt ? OrdType::Limit : OrdType::Market,
                               c.outside_rth, c.qty, lmt ? c.limit_price : 0.0,
                               0.0, c.take_profit, c.stop_loss};
                next_is_manual = true;
                ctx.set_manual(true);   // bypass the entry gate + halt quarantine
                const uint64_t id = ctx.submit_order(r);
                ctx.set_manual(false);
                next_is_manual = false;
                push_log(id ? "live: manual " + std::string(c.buy ? "BUY " : "SELL ") +
                                  std::to_string(static_cast<long long>(c.qty)) +
                                  " " + symbol_name(sid) +
                                  (lmt ? " @ " + std::to_string(c.limit_price) : " (mkt)") +
                                  (c.outside_rth ? " [outside RTH]" : "") + " submitted (#" +
                                  std::to_string(id) + ")"
                            : "live: manual order rejected");
                break;
            }
            }
        }
        if (had_cmd) publish();

        // Broker events (fills/cancels/rejects) arrive on their own clock,
        // independent of market data.
        if (broker) {
            EngineEvent bev;
            bool any = false;
            while (broker->poll_event(bev)) {
                any = true;
                if (bev.type == static_cast<uint16_t>(EvType::Fill)) {
                    const Fill f{bev.u.fill.order_id, bev.symbol_id,
                                 static_cast<Side>(bev.u.fill.side), {},
                                 bev.ts_event_ns ? bev.ts_event_ns : rt.now_ns(),
                                 bev.u.fill.price, bev.u.fill.qty, bev.u.fill.fee};
                    apply_fill(f);
                    // A held (adopted) symbol whose position just closed — its
                    // adopted stop/TP filled: re-init its strategy and resume.
                    const uint32_t fsid = f.symbol_id;
                    if (fsid >= 1 && fsid <= n_sym && adopt_hold[fsid - 1] &&
                        pf.position(fsid).qty == 0.0) {
                        adopt_hold[fsid - 1] = 0;
                        if (strategies[fsid - 1]) {
                            strat_halted[fsid - 1] = 0;
                            ctx.set_current_event_tsc(0);
                            ctx.set_current_symbol(fsid);
                            strategies[fsid - 1]->on_init(ctx);
                        }
                        push_log("live: " + cfg.symbols[fsid - 1] +
                                 " adopted position closed — strategy resumed");
                    }
                } else if (bev.type == static_cast<uint16_t>(EvType::OrderCancel)) {
                    const bool rejected = (bev.flags & kEvFlagRejected) != 0;
                    // Pull the reason once per event (take_reject erases it), so
                    // /diag can show why, not just that, an order was rejected.
                    RejectReason rr;
                    if (rejected) rr = broker->take_reject(bev.u.order.order_id);
                    // A rejection with no cause is unactionable, so there is no
                    // longer such a thing: an adapter that says nothing gets the
                    // generic sentence, which at least names the layer. On
                    // 2026-08-13 this branch stored a blank and /diag printed it.
                    if (rejected) {
                        if (rr.cause == RejectCause::None)
                            rr.cause = rr.code ? RejectCause::BrokerRejected
                                               : RejectCause::BrokerRefused;
                        if (rr.message.empty()) rr.message = reject_cause_text(rr.cause);
                    }
                    // A specific reason may overwrite a generic one, but never
                    // the other way round: a late reason-less event must not
                    // wipe a captured "201 Exchange is closed".
                    const bool specific = rr.cause != RejectCause::BrokerRefused;
                    OrderRequest refused{};   // rebuilt for the refusal table
                    bool refused_known = false;
                    for (auto& o : orders)
                        if (o.id == bev.u.order.order_id) {
                            o.status =
                                rejected ? OrderStatus::Rejected : OrderStatus::Cancelled;
                            if (rejected &&
                                (specific || o.reject_cause == RejectCause::None)) {
                                o.reject_cause = rr.cause;
                                o.reject_code = rr.code;
                                o.reject_msg = rr.message;
                            }
                            refused.symbol_id = o.symbol_id;
                            refused.side = static_cast<Side>(o.side);
                            refused.qty = o.qty;
                            refused_known = true;
                            orders_dirty = true;
                        }
                    // Count it alongside the local refusals, so "how many
                    // orders did this session fail to place, and why" is ONE
                    // table rather than a blotter scan. This is also what gives
                    // a broker-rejected EXIT its Critical page even when the
                    // adapter did not tag it protective — the naked-position
                    // net below only fires on kEvFlagProtective, and a plain
                    // signal exit refused by IBKR is just as naked.
                    //
                    // UNCONDITIONAL since 0.23.1. It used to require a blotter
                    // row, and two whole classes of rejection do not have one:
                    // a bracket's take-profit/stop legs get their ids inside
                    // the adapter (tws_broker.cpp handle_submit) and are never
                    // returned from submit(), and a resting leg's row can age
                    // out of the 200-entry ring on a multi-day session. Those
                    // rejections produced NOTHING — no refusals row, no
                    // reject_count, no refused_exits, no page above Warning —
                    // and a rejected take-profit leg is precisely the exit this
                    // path was added to cover.
                    if (rejected) {
                        // The blotter knew the symbol; without a row, the event
                        // does (push_reject carries it for every reject now).
                        if (!refused_known) refused.symbol_id = bev.symbol_id;
                        const uint32_t rsid = refused.symbol_id;
                        // With no row there is no side and no qty to infer from,
                        // so the POSITION decides: an unidentifiable rejection on
                        // a symbol that holds a position is treated as an exit.
                        // Deliberately asymmetric — over-calling it costs one
                        // page, under-calling it costs the naked position that
                        // this project's two worst incidents both were.
                        const bool unknown_on_open =
                            !refused_known && rsid >= 1 && rsid <= n_sym &&
                            pf.position(rsid).qty != 0.0;
                        note_refusal(refused, rr, /*record=*/!refused_known,
                                     /*known_exit=*/(bev.flags & kEvFlagProtective) != 0 ||
                                         unknown_on_open);
                    }
                    // "refused", not "rejected": the tagged line note_refusal
                    // just emitted is what decides the alert tier, and this one
                    // carries the id and the broker's code for the log. While it
                    // said "rejected" it matched classify_alert's generic rule
                    // and beeped Warning for every refusal — including the
                    // benign refused ENTRY the policy above deliberately rates
                    // Info, which drew TWO pages per order.
                    std::string line = "live: order #" +
                                       std::to_string(bev.u.order.order_id) +
                                       (rejected ? " refused by broker" : " cancelled");
                    if (rejected) {
                        line += " [";
                        line += reject_cause_slug(rr.cause);
                        line += " ";
                        if (rr.code) line += std::to_string(rr.code) + " ";
                        line += rr.message + "]";
                    }
                    push_log(line);
                    // Tell the owning strategy before reacting below: this is
                    // the only notice it gets that the id is dead, and the
                    // protective-reject path may halt the symbol immediately.
                    notify_order_end(bev.u.order.order_id, bev.symbol_id,
                                     rejected ? OrderEndReason::Rejected
                                              : OrderEndReason::Cancelled,
                                     rejected ? rr.code : 0);
                    // Naked-position safety net: a protective stop rejected by the
                    // broker leaves the position it guarded exposed. Flatten that
                    // symbol at market and halt just it (the broker sets symbol_id
                    // + kEvFlagProtective on such rejects).
                    if (rejected && (bev.flags & kEvFlagProtective) &&
                        bev.symbol_id >= 1 && bev.symbol_id <= n_sym) {
                        const uint32_t sid = bev.symbol_id;
                        const std::string sym = symbol_name(sid);
                        if (pf.position(sid).qty != 0.0 && !halted &&
                            !strat_halted[sid - 1]) {
                            flatten_halt_symbol(
                                sid, "PROTECTIVE STOP REJECTED on " + sym +
                                         " — position was naked; flattened + halted symbol");
                            check_risk();
                        } else {
                            push_log("live: protective stop rejected on " + sym +
                                     " but position already flat/halted — no action");
                        }
                    }
                } else if (bev.type == static_cast<uint16_t>(EvType::PosSnap)) {
                    // Only adopt while reconciling. A snapshot that lands after
                    // reconciliation ended (failsafe lifted the gate, or a stray
                    // late replay) would overwrite a position the strategy has
                    // since traded and wrongly re-pause it — reconciliation is a
                    // one-shot at session start.
                    if (!reconciling) {
                        push_log("live: ignoring late broker position snapshot for " +
                                 symbol_name(bev.symbol_id) +
                                 " (reconciliation already ended)");
                    } else {
                        pf.seed_position(bev.symbol_id, bev.u.pos.qty,
                                         bev.u.pos.avg_price);
                        risk_base_eq = risk_high_eq = pf.equity();   // new baseline
                        // A nonzero adopted position: pause that symbol's strategy
                        // until it goes flat (its adopted stop/TP protects it).
                        const bool hold = bev.u.pos.qty != 0.0 && bev.symbol_id >= 1 &&
                                          bev.symbol_id <= n_sym;
                        if (hold) adopt_hold[bev.symbol_id - 1] = 1;
                        char buf[128];
                        std::snprintf(
                            buf, sizeof(buf),
                            "live: adopted broker position %s %+.0f @ %.2f%s",
                            symbol_name(bev.symbol_id).c_str(), bev.u.pos.qty,
                            bev.u.pos.avg_price, hold ? " — holding until flat" : "");
                        push_log(buf);
                    }
                } else if (bev.type == static_cast<uint16_t>(EvType::AcctSnap)) {
                    if (!reconciling) {
                        push_log("live: ignoring late broker cash snapshot "
                                 "(reconciliation already ended)");
                    } else {
                        pf.set_cash(bev.u.acct.cash);
                        risk_base_eq = risk_high_eq = pf.equity();   // new baseline
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "live: cash reconciled to %.2f",
                                      bev.u.acct.cash);
                        push_log(buf);
                    }
                } else if (bev.type == static_cast<uint16_t>(EvType::OrderNew)) {
                    // A resting order adopted from the broker at reconnect — book
                    // it Working so /diag + the blotter show it (and a held symbol
                    // reads protected, not naked). Its later fill/cancel maps back
                    // through the adapter's id table like any live order.
                    const OrderRequest r{bev.symbol_id,
                                         static_cast<Side>(bev.u.order.side),
                                         static_cast<OrdType>(bev.u.order.ord_type), {},
                                         bev.u.order.qty, bev.u.order.limit_price,
                                         0.0, 0.0, 0.0};
                    record_order(r, bev.u.order.order_id);
                    push_log("live: adopted broker order #" +
                             std::to_string(bev.u.order.order_id) + " (" +
                             symbol_name(bev.symbol_id) + ")");
                } else if (bev.type == static_cast<uint16_t>(EvType::ReconcileEnd)) {
                    reconciling = false;
                    int held = 0;
                    for (const char h : adopt_hold) held += h ? 1 : 0;
                    // The off-lineup count is NOT decoration. This line used to
                    // read "0 symbol(s) held until flat" on an account holding
                    // 20 NVDA shares that reconciliation had silently discarded
                    // — for 24 days — because `held` counts adopted SESSION
                    // symbols and an unrecognised one is never adopted. It is
                    // the one line an operator sees at every session start, so
                    // it must not be able to say "complete" about a book it did
                    // not fully account for. See engine/reconcile_policy.h.
                    const uint32_t off = bev.u.recon.offlineup;
                    push_log("live: broker reconciliation complete — " +
                             std::to_string(held) + " symbol(s) held until flat" +
                             (off ? ", " + std::to_string(off) +
                                        " BROKER POSITION(S) OUTSIDE THIS LINEUP "
                                        "- not adopted, not closeable here"
                                  : std::string()));
                }
            }
            if (any) {
                idle = 0;
                check_risk();
                publish();
            }
        }

        EngineEvent ev;
        if (feed_ring_->try_pop(ev) || md_ring_->try_pop(ev)) {
            idle = 0;
            ++ticks;
            capture.write(ev);
            const int64_t now = rt.now_ns();
            last_md_ns = now;

            if (ev.type == static_cast<uint16_t>(EvType::Bar)) {
                // Backfilled bar (feed outage recovery): keep the strategy's
                // indicators continuous, but never fill or mark positions
                // against stale prices.
                //
                // The bar's close IS a usable price for sizing, and setting it
                // is not marking: ctx's last price feeds the price band, the
                // buying-power check and the notional clamp only. Without it a
                // strategy ordering off a backfill bar on a symbol that has not
                // ticked this session was priced at 0 — both gates blind, and
                // since 0.23.0 an unpriceable increase is refused outright with
                // price_unavailable. run_replay does the same at the matching
                // branch; when only one of them did, a capture no longer
                // reproduced the live session it was taken from, which is the
                // whole point of the capture.
                if (ev.symbol_id >= 1 && ev.symbol_id <= n_sym)
                    ctx.set_last_price(ev.symbol_id, ev.u.bar.close);
                if (!halted && ev.symbol_id >= 1 && ev.symbol_id <= n_sym &&
                    !strat_halted[ev.symbol_id - 1] && !reconciling &&
                    !adopt_hold[ev.symbol_id - 1]) {
                    ctx.set_current_event_tsc(0);
                    const Bar b{ev.ts_event_ns, ev.u.bar.open, ev.u.bar.high,
                                ev.u.bar.low, ev.u.bar.close, ev.u.bar.volume};
                    if (IStrategy* s = strat_for(ev.symbol_id)) {
                        ctx.set_current_symbol(ev.symbol_id);
                        guarded(ev.symbol_id, [&] { s->on_bar(ctx, ev.symbol_id, b); });
                    }
                }
                continue;
            }

            const uint32_t sid = ev.symbol_id;
            ctx.set_current_event_tsc(ev.ts_ingest_tsc);
            const double price = ev.u.tick.price;
            if (sid > 0 && sid <= n_sym) last_price[sid - 1] = price;
            last_ts_ms = ev.ts_event_ns / 1'000'000;
            ctx.set_last_price(sid, price);

            if (!broker) {
                fills.clear();
                exec.on_price(sid, price, now, fills);
                for (const Fill& f : fills) apply_fill(f);
                // Before on_tick: a strategy deciding on stale in-flight state
                // is exactly the wedge this exists to prevent. Catches the OCO
                // sibling this fill just killed, and anything on_fill cancelled.
                drain_sim_cancels();
            }
            pf.mark(sid, price);

            if (!halted && sid >= 1 && sid <= n_sym && !strat_halted[sid - 1] &&
                !reconciling && !adopt_hold[sid - 1])
                if (IStrategy* s = strat_for(sid)) {
                    Tick t{ev.ts_event_ns, price, ev.u.tick.size, ev.u.tick.bid,
                           ev.u.tick.ask};
                    ctx.set_current_symbol(sid);
                    guarded(sid, [&] { s->on_tick(ctx, sid, t); });
                }
            roll_bar(sid, ev.ts_event_ns, price);
            if (!broker) drain_sim_cancels();   // cancels made inside on_tick/on_bar
            check_risk();
            if (halted) publish();          // a risk halt must surface instantly
            else publish_throttled(now);
            // Outside tick processing there is no tick to measure from:
            // manual/broker-driven submits must not record bogus latencies.
            ctx.set_current_event_tsc(0);
        } else {
            ++idle;
            // Burst ended with a coalesced update still unpublished: flush it
            // so the UI never shows stale state longer than one interval.
            if (snap_dirty && rt.now_ns() - last_pub_ns >= kPublishInterval) {
                publish();
                last_pub_ns = rt.now_ns();
                snap_dirty = false;
            }
            if (cfg.busy_spin) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
                // Stay scheduler-friendly without ever timer-sleeping: yield
                // once in a while so same-core threads aren't starved.
                if ((idle & 0x3FFF) == 0) Sleep(0);
            } else if (idle < 5'000) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
            } else if (idle < 20'000) {
                Sleep(0);
            } else {
                Sleep(5);   // delayed-quote cadence: don't pin a core for nothing
            }
            // Stale-feed halt: holding a position blind is the one state a
            // trading loop must never sit in quietly. Spin mode checks once
            // per ~16k iterations (µs apart) to keep the spin pure; sleep
            // mode checks every 5 ms tier iteration as before.
            const bool stale_due =
                cfg.busy_spin ? (idle & 0x3FFF) == 0 : idle >= 20'000;
            if (stale_due && !halted && cfg.risk.stale_feed_sec > 0 &&
                ticks > 0 &&
                rt.now_ns() - last_md_ns >
                    static_cast<int64_t>(cfg.risk.stale_feed_sec) * 1'000'000'000) {
                bool open_pos = false;
                for (size_t i = 0; i < n_sym && !open_pos; ++i)
                    open_pos = pf.position(static_cast<uint32_t>(i + 1)).qty != 0.0;
                if (open_pos) {
                    kill_all("RISK HALT (feed stale " +
                             std::to_string(cfg.risk.stale_feed_sec) +
                             "s with open position)");
                    publish();
                }
            }
            // Same idle cadence: cheap (one localtime, then a day-stamp check
            // short-circuits it for the rest of the day).
            if (stale_due) eod_backstop();
        }
    }

    // Real orders must not outlive the session that's watching them — EXCEPT a
    // keep-positions restart, which deliberately leaves the position open and
    // relies on its resting stop/TP surviving to be re-adopted + held on the
    // next start. Cancelling them here would strand that position naked (no
    // protective order) and paused (adopt_hold never lifts without a fill).
    if (broker && !keep_broker_orders) {
        broker->cancel_all();
        push_log("live: cancelled open broker orders on stop");
    } else if (broker) {
        push_log("live: keeping open broker orders for restart re-adoption");
    }

    if (capture.is_open()) {
        push_log("live: captured " + std::to_string(capture.written()) + " events to " +
                 cfg.capture_path);
        capture.close();
    }

    for (size_t i = 0; i < strategies.size(); ++i)
        if (strategies[i] && first_use(i)) {
            ctx.set_current_symbol(static_cast<uint32_t>(i + 1));
            strategies[i]->on_stop(ctx);
        }
    {
        std::lock_guard lock(snap_mu_);
        snap_.running = false;
        snap_.halted = halted;
    }
    live_running_ = false;
    push_log("live: stopped");
}

} // namespace tt
