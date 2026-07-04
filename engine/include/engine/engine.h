#pragma once
// Backtest engine: a feed thread replays bars (plus synthetic intrabar
// O/H/L/C ticks) into md_ring at full speed; the engine thread consumes,
// advances the BacktestClock, runs the strategy, simulates execution, and
// tracks the portfolio. One consumer thread + a deterministic clock + seeded
// RNG => bit-identical reruns. The live paper path (Phase 4) reuses this
// loop with a RealTimeClock — the clock is the only difference.

#include "engine/clock.h"
#include "engine/events.h"
#include "engine/exec_sim.h"
#include "engine/latency.h"
#include "engine/portfolio.h"
#include "engine/spsc_ring.h"
#include "tt/strategy_api.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt {

struct BacktestConfig {
    std::string symbol;
    std::vector<Bar> bars;                  // ts_ns ascending (bar open times)
    double initial_cash = 100'000.0;
    ExecParams exec{};
    std::map<std::string, double> params;   // strategy parameters
    bool synth_ticks = true;                // O/H/L/C intrabar ticks for limit fills
};

struct TradeRow {
    int64_t ts_ns;
    uint8_t side;      // tt::Side
    double qty, price, fee;
};

struct BacktestResult {
    std::string symbol;
    double initial_cash = 0, final_equity = 0, total_return = 0;
    double max_drawdown = 0, sharpe = 0, win_rate = 0;
    int trades = 0, wins = 0, losses = 0;
    std::vector<double> eq_ts;      // epoch seconds (ImPlot time axis)
    std::vector<double> eq_val;
    std::vector<TradeRow> fills;
    int64_t lat_p50 = 0, lat_p99 = 0, lat_max = 0;
    uint64_t lat_count = 0;
    double duration_ms = 0;
    uint64_t events = 0;
};

// ---------------------------------------------------------------- live mode

struct RiskLimits {
    double max_order_qty = 1'000;
    double max_position_qty = 5'000;
    double price_band_pct = 0.20;   // limit orders within ±20% of last trade
};

struct LiveConfig {
    std::string symbol;
    double initial_cash = 100'000.0;
    ExecParams exec{};
    std::map<std::string, double> params;
    RiskLimits risk{};
    int bar_seconds = 60;           // tick->bar aggregation for on_bar
};

enum class OrderStatus : uint8_t { Working = 0, Filled, Cancelled, Rejected };

struct OrderRecord {
    uint64_t id = 0;
    int64_t ts_ns = 0;
    uint8_t side = 0, type = 0;
    OrderStatus status = OrderStatus::Working;
    double qty = 0, limit_price = 0, fill_price = 0, fee = 0;
    bool manual = false;
};

struct LiveSnapshot {
    bool running = false, halted = false;
    std::string symbol;
    double cash = 0, equity = 0, last_price = 0;
    Position position{};
    std::vector<OrderRecord> orders;   // newest last, capped
    uint64_t ticks = 0, dropped_ticks = 0;
    int64_t last_tick_ts_ms = 0;
};

class Engine {
public:
    Engine();
    ~Engine();

    // Strategy is caller-owned and must outlive the run; on_init must fully
    // reset its state. Returns false if a backtest is already running.
    bool start_backtest(BacktestConfig cfg, IStrategy* strategy);

    bool running() const { return running_.load(std::memory_order_relaxed); }
    // True exactly once per finished run.
    bool take_result(BacktestResult& out);
    // Engine/strategy log lines, drained by the UI each frame.
    bool pop_log(std::string& out);

    // ---- live paper trading ----
    bool start_live(LiveConfig cfg, IStrategy* strategy);
    void stop_live();                       // graceful: on_stop, joins the thread
    bool live_running() const { return live_running_.load(std::memory_order_relaxed); }
    std::string live_symbol() const;
    // IPC thread: feed a delayed quote into the live engine.
    void push_live_tick(int64_t ts_ms, double price, double day_volume);
    // UI thread: async commands consumed by the engine thread.
    void request_cancel(uint64_t order_id);
    void submit_manual(bool buy, double qty);
    void kill_switch();                     // cancel all + flatten + halt strategy
    LiveSnapshot live_snapshot() const;

private:
    friend class EngineCtx;
    struct LiveCmd {
        enum : uint8_t { Stop = 1, Cancel, Kill, Manual } type = Stop;
        uint8_t buy = 0;
        uint64_t order_id = 0;
        double qty = 0;
    };
    void run(BacktestConfig cfg, IStrategy* strategy);
    void run_live(LiveConfig cfg, IStrategy* strategy);
    void push_log(std::string line);

    // Heap-allocated: the 4 MiB buffer must never land on a caller's stack.
    using MdRing = SpscRing<EngineEvent, 1 << 16>;
    std::unique_ptr<MdRing> md_ring_ = std::make_unique<MdRing>();
    std::thread engine_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> has_result_{false};

    std::mutex result_mu_;
    BacktestResult result_;

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    // ---- live state ----
    using CmdRing = SpscRing<LiveCmd, 1 << 12>;
    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::thread live_thread_;
    std::atomic<bool> live_running_{false};
    std::atomic<uint64_t> dropped_ticks_{0};
    mutable std::mutex snap_mu_;
    LiveSnapshot snap_;
};

} // namespace tt
