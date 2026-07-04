#include "engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

namespace tt {

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
} // namespace

// IStrategyContext implementation — lives on the engine thread's stack for
// the duration of one run (backtest or live).
class EngineCtx final : public IStrategyContext {
public:
    using NowFn = std::function<int64_t()>;
    using OrderHook = std::function<void(const OrderRequest&, uint64_t id)>;

    EngineCtx(Engine& eng, const std::map<std::string, double>& params,
              const std::string& symbol, NowFn now, ExecSim& exec, Portfolio& pf,
              LatencyHistogram& lat, const RiskLimits* risk = nullptr,
              OrderHook on_order = {})
        : eng_(eng), params_(params), now_(std::move(now)), exec_(exec), pf_(pf),
          lat_(lat), risk_(risk), on_order_(std::move(on_order)) {
        symbols_.push_back(symbol);
    }

    uint64_t submit_order(const OrderRequest& r) noexcept override {
        if (r.qty <= 0.0 || r.symbol_id == 0 || r.symbol_id > symbols_.size())
            return 0;
        if (risk_ && !risk_ok(r)) {
            if (on_order_) on_order_(r, 0);   // recorded as Rejected
            return 0;
        }
        const uint64_t id = exec_.submit(r, now_());
        if (id && cur_event_tsc_)
            lat_.record(tsc::to_ns(rdtsc() - cur_event_tsc_));  // tick -> order
        if (on_order_) on_order_(r, id);
        return id;
    }
    bool cancel_order(uint64_t order_id) noexcept override {
        return exec_.cancel(order_id);
    }
    Position position(uint32_t symbol_id) const noexcept override {
        return pf_.position(symbol_id);
    }
    double cash() const noexcept override { return pf_.cash(); }
    int64_t now_ns() const noexcept override { return now_(); }

    uint32_t symbol_id(const char* symbol) noexcept override {
        for (size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == symbol) return static_cast<uint32_t>(i + 1);
        symbols_.emplace_back(symbol);
        return static_cast<uint32_t>(symbols_.size());
    }
    double param(const char* name, double fallback) const noexcept override {
        auto it = params_.find(name);
        return it != params_.end() ? it->second : fallback;
    }
    void log(int level, const char* msg) noexcept override {
        static constexpr const char* kLevels[] = {"debug", "info", "warn", "error"};
        const int l = level < 0 ? 0 : (level > 3 ? 3 : level);
        eng_.push_log(std::string("[strategy ") + kLevels[l] + "] " + msg);
    }

    void set_current_event_tsc(int64_t tsc) { cur_event_tsc_ = tsc; }
    void set_last_price(double p) { last_price_ = p; }

private:
    bool risk_ok(const OrderRequest& r) noexcept {
        if (r.qty > risk_->max_order_qty) {
            eng_.push_log("risk: order qty exceeds limit, rejected");
            return false;
        }
        const double pos = pf_.position(r.symbol_id).qty;
        const double dir = r.side == Side::Buy ? 1.0 : -1.0;
        if (std::abs(pos + dir * r.qty) > risk_->max_position_qty) {
            eng_.push_log("risk: resulting position exceeds limit, rejected");
            return false;
        }
        if (r.type == OrdType::Limit && last_price_ > 0.0 &&
            std::abs(r.limit_price - last_price_) / last_price_ > risk_->price_band_pct) {
            eng_.push_log("risk: limit price outside band, rejected (fat finger?)");
            return false;
        }
        return true;
    }

    Engine& eng_;
    const std::map<std::string, double>& params_;
    NowFn now_;
    ExecSim& exec_;
    Portfolio& pf_;
    LatencyHistogram& lat_;
    const RiskLimits* risk_;
    OrderHook on_order_;
    std::vector<std::string> symbols_;
    int64_t cur_event_tsc_ = 0;
    double last_price_ = 0.0;
};

Engine::Engine() { tsc::calibrate(); }

Engine::~Engine() {
    stop_live();
    if (engine_thread_.joinable()) engine_thread_.join();
}

void Engine::push_log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back(std::move(line));
    while (logs_.size() > 1000) logs_.pop_front();
}

bool Engine::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
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
    EngineCtx ctx(*this, cfg.params, cfg.symbol,
                  [&clock] { return clock.now_ns(); }, exec, pf, lat);

    BacktestResult res;
    res.symbol = cfg.symbol;
    res.initial_cash = cfg.initial_cash;
    res.eq_ts.reserve(cfg.bars.size());
    res.eq_val.reserve(cfg.bars.size());

    const int64_t dt = median_gap_ns(cfg.bars);
    constexpr uint32_t kSymbolId = 1;

    strategy->on_init(ctx);

    // Feed thread: replays the series at full speed. Backpressure by
    // spinning — the backtest must be lossless.
    std::thread feed([this, &cfg, dt] {
        auto push = [this](EngineEvent& ev) {
            ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
            while (!md_ring_->try_push(ev)) { /* spin: consumer is draining */ }
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

    // Engine loop: single consumer, deterministic order.
    std::vector<Fill> fills;
    fills.reserve(16);
    EngineEvent ev;
    uint64_t events = 0;
    for (;;) {
        if (!md_ring_->try_pop(ev)) {
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

        fills.clear();
        exec.on_price(ev.symbol_id, price, clock.now_ns(), fills);
        for (const Fill& f : fills) {
            pf.apply(f);
            res.fills.push_back(TradeRow{f.ts_ns, static_cast<uint8_t>(f.side),
                                         f.qty, f.price, f.fee});
            strategy->on_fill(ctx, f);
        }
        pf.mark(ev.symbol_id, price);

        if (ev.type == static_cast<uint16_t>(EvType::Tick)) {
            Tick t{ev.ts_event_ns, ev.u.tick.price, ev.u.tick.size,
                   ev.u.tick.bid, ev.u.tick.ask};
            strategy->on_tick(ctx, ev.symbol_id, t);
        } else if (ev.type == static_cast<uint16_t>(EvType::Bar)) {
            Bar b{ev.ts_event_ns - dt, ev.u.bar.open, ev.u.bar.high,
                  ev.u.bar.low, ev.u.bar.close, ev.u.bar.volume};
            strategy->on_bar(ctx, ev.symbol_id, b);
            res.eq_ts.push_back(static_cast<double>(ev.ts_event_ns) / 1e9);
            res.eq_val.push_back(pf.equity());
        }
    }
    feed.join();
    strategy->on_stop(ctx);

    // ---- statistics ----
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
            const double dt_sec = static_cast<double>(dt) / 1e9;
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

// ------------------------------------------------------------------- live

std::string Engine::live_symbol() const {
    std::lock_guard lock(snap_mu_);
    return snap_.symbol;
}

LiveSnapshot Engine::live_snapshot() const {
    std::lock_guard lock(snap_mu_);
    LiveSnapshot s = snap_;
    s.dropped_ticks = dropped_ticks_.load(std::memory_order_relaxed);
    return s;
}

void Engine::push_live_tick(int64_t ts_ms, double price, double day_volume) {
    if (!live_running_.load(std::memory_order_relaxed)) return;
    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::Tick);
    ev.symbol_id = 1;
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
    cmd_ring_->try_push(LiveCmd{LiveCmd::Cancel, 0, order_id, 0});
}

void Engine::submit_manual(bool buy, double qty) {
    cmd_ring_->try_push(LiveCmd{LiveCmd::Manual, static_cast<uint8_t>(buy), 0, qty});
}

void Engine::kill_switch() {
    cmd_ring_->try_push(LiveCmd{LiveCmd::Kill, 0, 0, 0});
}

void Engine::stop_live() {
    if (live_running_.load(std::memory_order_relaxed))
        while (!cmd_ring_->try_push(LiveCmd{LiveCmd::Stop, 0, 0, 0})) Sleep(1);
    if (live_thread_.joinable()) live_thread_.join();
}

bool Engine::start_live(LiveConfig cfg, IStrategy* strategy) {
    if (running_.load(std::memory_order_relaxed)) return false;
    if (live_running_.exchange(true)) return false;
    if (live_thread_.joinable()) live_thread_.join();

    // Fresh rings for the new session.
    EngineEvent ev;
    while (md_ring_->try_pop(ev)) {}
    LiveCmd c;
    while (cmd_ring_->try_pop(c)) {}
    dropped_ticks_ = 0;
    {
        std::lock_guard lock(snap_mu_);
        snap_ = LiveSnapshot{};
        snap_.running = true;
        snap_.symbol = cfg.symbol;
        snap_.cash = cfg.initial_cash;
        snap_.equity = cfg.initial_cash;
    }
    live_thread_ = std::thread(
        [this, cfg = std::move(cfg), strategy]() mutable { run_live(std::move(cfg), strategy); });
    return true;
}

void Engine::run_live(LiveConfig cfg, IStrategy* strategy) {
    RealTimeClock rt;
    ExecSim exec(cfg.exec);
    Portfolio pf(cfg.initial_cash);
    LatencyHistogram lat;
    bool halted = false;

    std::vector<OrderRecord> orders;   // engine-thread master copy
    bool next_is_manual = false;
    auto record_order = [&](const OrderRequest& r, uint64_t id) {
        OrderRecord rec;
        rec.id = id;
        rec.ts_ns = rt.now_ns();
        rec.side = static_cast<uint8_t>(r.side);
        rec.type = static_cast<uint8_t>(r.type);
        rec.status = id ? OrderStatus::Working : OrderStatus::Rejected;
        rec.qty = r.qty;
        rec.limit_price = r.limit_price;
        rec.manual = next_is_manual;
        orders.push_back(rec);
        if (orders.size() > 200) orders.erase(orders.begin());
    };

    EngineCtx ctx(*this, cfg.params, cfg.symbol, [&rt] { return rt.now_ns(); },
                  exec, pf, lat, &cfg.risk, record_order);

    uint64_t ticks = 0;
    double last_price = 0.0;
    int64_t last_ts_ms = 0;

    auto publish = [&] {
        std::lock_guard lock(snap_mu_);
        snap_.running = true;
        snap_.halted = halted;
        snap_.symbol = cfg.symbol;
        snap_.cash = pf.cash();
        snap_.equity = pf.equity();
        snap_.last_price = last_price;
        snap_.position = pf.position(1);
        snap_.orders = orders;
        snap_.ticks = ticks;
        snap_.last_tick_ts_ms = last_ts_ms;
    };

    strategy->on_init(ctx);
    push_log("live: paper trading " + cfg.symbol + " started (bar " +
             std::to_string(cfg.bar_seconds) + "s)");

    // Tick -> bar aggregation so bar-based strategies work on the live feed.
    const int64_t bar_ns = static_cast<int64_t>(cfg.bar_seconds) * 1'000'000'000;
    Bar cur_bar{};
    bool bar_open = false;
    auto roll_bar = [&](int64_t ts, double px) {
        if (bar_open && ts >= cur_bar.ts_ns + bar_ns) {
            if (!halted) strategy->on_bar(ctx, 1, cur_bar);
            bar_open = false;
        }
        if (!bar_open) {
            bar_open = true;
            cur_bar = Bar{ts / bar_ns * bar_ns, px, px, px, px, 0.0};
            return;
        }
        cur_bar.high = std::max(cur_bar.high, px);
        cur_bar.low = std::min(cur_bar.low, px);
        cur_bar.close = px;
    };

    std::vector<Fill> fills;
    fills.reserve(8);
    int idle = 0;
    bool stop = false;
    while (!stop) {
        LiveCmd c;
        bool had_cmd = false;
        while (cmd_ring_->try_pop(c)) {
            had_cmd = true;
            switch (c.type) {
            case LiveCmd::Stop:
                stop = true;
                break;
            case LiveCmd::Cancel:
                if (exec.cancel(c.order_id)) {
                    for (auto& o : orders)
                        if (o.id == c.order_id) o.status = OrderStatus::Cancelled;
                    push_log("live: cancelled order #" + std::to_string(c.order_id));
                }
                break;
            case LiveCmd::Kill: {
                halted = true;
                for (uint64_t id : exec.cancel_all())
                    for (auto& o : orders)
                        if (o.id == id) o.status = OrderStatus::Cancelled;
                const double pos = pf.position(1).qty;
                if (pos != 0.0) {
                    // Flatten bypasses risk checks — closing must always work.
                    OrderRequest r{1, pos > 0 ? Side::Sell : Side::Buy,
                                   OrdType::Market, {}, std::abs(pos), 0.0};
                    next_is_manual = true;
                    record_order(r, exec.submit(r, rt.now_ns()));
                    next_is_manual = false;
                }
                push_log("live: KILL SWITCH — all orders cancelled, flattening, strategy halted");
                break;
            }
            case LiveCmd::Manual: {
                if (last_price <= 0.0) {
                    push_log("live: manual order rejected (no market data yet)");
                    break;
                }
                OrderRequest r{1, c.buy ? Side::Buy : Side::Sell, OrdType::Market, {},
                               c.qty, 0.0};
                next_is_manual = true;
                const uint64_t id = ctx.submit_order(r);
                next_is_manual = false;
                push_log(id ? "live: manual " + std::string(c.buy ? "BUY " : "SELL ") +
                                  std::to_string(static_cast<long long>(c.qty)) +
                                  " submitted (#" + std::to_string(id) + ")"
                            : "live: manual order rejected");
                break;
            }
            }
        }
        if (had_cmd) publish();

        EngineEvent ev;
        if (md_ring_->try_pop(ev)) {
            idle = 0;
            ++ticks;
            const int64_t now = rt.now_ns();
            ctx.set_current_event_tsc(ev.ts_ingest_tsc);
            const double price = ev.u.tick.price;
            last_price = price;
            last_ts_ms = ev.ts_event_ns / 1'000'000;
            ctx.set_last_price(price);

            fills.clear();
            exec.on_price(1, price, now, fills);
            for (const Fill& f : fills) {
                pf.apply(f);
                for (auto& o : orders)
                    if (o.id == f.order_id) {
                        o.status = OrderStatus::Filled;
                        o.fill_price = f.price;
                        o.fee = f.fee;
                    }
                char buf[96];
                std::snprintf(buf, sizeof(buf), "live: fill #%llu %s %.0f @ %.2f",
                              static_cast<unsigned long long>(f.order_id),
                              f.side == Side::Buy ? "BUY" : "SELL", f.qty, f.price);
                push_log(buf);
                if (!halted) strategy->on_fill(ctx, f);
            }
            pf.mark(1, price);

            if (!halted) {
                Tick t{ev.ts_event_ns, price, ev.u.tick.size, ev.u.tick.bid, ev.u.tick.ask};
                strategy->on_tick(ctx, 1, t);
            }
            roll_bar(ev.ts_event_ns, price);
            publish();
        } else {
            ++idle;
            if (idle < 5'000) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
            } else if (idle < 20'000) {
                Sleep(0);
            } else {
                Sleep(5);   // delayed-quote cadence: don't pin a core for nothing
            }
        }
    }

    strategy->on_stop(ctx);
    {
        std::lock_guard lock(snap_mu_);
        snap_.running = false;
        snap_.halted = halted;
    }
    live_running_ = false;
    push_log("live: stopped");
}

} // namespace tt
