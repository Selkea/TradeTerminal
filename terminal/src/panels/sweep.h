#pragma once

#include "engine/engine.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tt::ui {

// Grid-search optimizer: runs a chosen strategy across a 1-D or 2-D
// parameter grid (serial backtests — the engine is fast and deterministic)
// and renders the metric surface as a heatmap.
class SweepPanel {
public:
    struct Request {
        std::string strat_key;   // strategy to sweep ("" = built-in SMA)
        std::string symbol, interval, range;
        double cash = 100'000.0;
        std::string px, py;      // parameter names; py empty = 1-D sweep
        double x0 = 0, x1 = 0;
        double y0 = 0, y1 = 0;
        int nx = 1, ny = 1;
        int metric = 0;          // index into kSweepMetrics
        // Walk-forward guard: optimize on the first (100-h)% of the data,
        // then score the best cell on the held-out tail it never saw.
        // 0 = off.
        double holdout_pct = 0;
    };

    // Owned by App (which runs the cells); read by the panel every frame.
    struct State {
        bool running = false;
        std::string px, py;
        std::vector<double> xs, ys;   // axis values (ys empty for 1-D)
        std::vector<double> vals;     // row-major [iy * xs.size() + ix]
        int done = 0, total = 0;
        int metric = 0;
        std::string label;            // "AAPL 1d 2y — SmaCrossover"
        double holdout_pct = 0;       // >0: holdout run follows the grid
        bool has_holdout = false;     // holdout_val is valid
        double holdout_val = 0;       // best cell's metric on unseen data
    };

    using RunFn = std::function<void(const Request&)>;
    using CancelFn = std::function<void()>;
    // Resolve a strategy key to its display name / current param values.
    using NameFn = std::function<std::string(const std::string& key)>;
    using ParamsFn =
        std::function<std::map<std::string, double>(const std::string& key)>;

    explicit SweepPanel(Engine& eng) : eng_(eng) {}
    // strat_keys: selectable strategies (loaded modules; built-in added in the
    // UI). The swept parameter names come from the picked strategy's params.
    void draw(bool* open, const std::vector<std::string>& strat_keys, const NameFn& name,
              const ParamsFn& params_of, const State& st, const RunFn& run,
              const CancelFn& cancel);

private:
    Engine& eng_;
    std::string strat_key_;   // "" = built-in SMA
    char sym_[16] = "AAPL";
    int interval_idx_ = 2;   // 1d
    int range_idx_ = 3;      // 2y
    double cash_ = 100'000.0;
    int px_idx_ = 0;
    int py_idx_ = 0;         // 0 = (none)
    double x0_ = 5, x1_ = 50;
    double y0_ = 20, y1_ = 200;
    int nx_ = 10, ny_ = 10;
    int metric_ = 0;
    double holdout_pct_ = 25.0;
    bool use_holdout_ = true;
};

// Higher is better for every metric except max drawdown.
constexpr const char* kSweepMetrics[] = {"Sharpe", "Return", "Max drawdown", "Win rate"};
inline bool sweep_metric_minimize(int m) { return m == 2; }
inline double sweep_metric_of(const BacktestResult& r, int m) {
    switch (m) {
    case 1: return r.total_return;
    case 2: return r.max_drawdown;
    case 3: return r.win_rate;
    default: return r.sharpe;
    }
}

} // namespace tt::ui
