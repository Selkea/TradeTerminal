#include "engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
// the duration of one run.
class EngineCtx final : public IStrategyContext {
public:
    EngineCtx(Engine& eng, const BacktestConfig& cfg, BacktestClock& clock,
              ExecSim& exec, Portfolio& pf, LatencyHistogram& lat)
        : eng_(eng), cfg_(cfg), clock_(clock), exec_(exec), pf_(pf), lat_(lat) {
        symbols_.push_back(cfg.symbol);
    }

    uint64_t submit_order(const OrderRequest& r) noexcept override {
        if (r.qty <= 0.0 || r.symbol_id == 0 || r.symbol_id > symbols_.size())
            return 0;
        const uint64_t id = exec_.submit(r, clock_.now_ns());
        if (id && cur_event_tsc_)
            lat_.record(tsc::to_ns(rdtsc() - cur_event_tsc_));  // tick -> order
        return id;
    }
    bool cancel_order(uint64_t order_id) noexcept override {
        return exec_.cancel(order_id);
    }
    Position position(uint32_t symbol_id) const noexcept override {
        return pf_.position(symbol_id);
    }
    double cash() const noexcept override { return pf_.cash(); }
    int64_t now_ns() const noexcept override { return clock_.now_ns(); }

    uint32_t symbol_id(const char* symbol) noexcept override {
        for (size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == symbol) return static_cast<uint32_t>(i + 1);
        symbols_.emplace_back(symbol);
        return static_cast<uint32_t>(symbols_.size());
    }
    double param(const char* name, double fallback) const noexcept override {
        auto it = cfg_.params.find(name);
        return it != cfg_.params.end() ? it->second : fallback;
    }
    void log(int level, const char* msg) noexcept override {
        static constexpr const char* kLevels[] = {"debug", "info", "warn", "error"};
        const int l = level < 0 ? 0 : (level > 3 ? 3 : level);
        eng_.push_log(std::string("[strategy ") + kLevels[l] + "] " + msg);
    }

    void set_current_event_tsc(int64_t tsc) { cur_event_tsc_ = tsc; }

private:
    Engine& eng_;
    const BacktestConfig& cfg_;
    BacktestClock& clock_;
    ExecSim& exec_;
    Portfolio& pf_;
    LatencyHistogram& lat_;
    std::vector<std::string> symbols_;
    int64_t cur_event_tsc_ = 0;
};

Engine::Engine() { tsc::calibrate(); }

Engine::~Engine() {
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
    EngineCtx ctx(*this, cfg, clock, exec, pf, lat);

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

} // namespace tt
