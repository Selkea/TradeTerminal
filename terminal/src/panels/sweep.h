#pragma once

#include "engine/engine.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tt::ui {

// Automatic optimizer: coordinate descent over a strategy's declared
// parameters — each pass sweeps every param 1-D across its range (others
// fixed at the best so far), a second pass refines in a narrower window,
// and the winner is applied back to the strategy's parameters.
class SweepPanel {
public:
    struct Request {
        std::string strat_key;   // strategy to optimize ("" = built-in SMA)
        std::string symbol, interval, range;
        double cash = 100'000.0;
        int metric = 0;          // index into kSweepMetrics
        // Walk-forward guard: optimize on the first (100-h)% of the data,
        // then score the winner on the held-out tail it never saw. 0 = off.
        double holdout_pct = 0;
    };

    // Owned by App (which runs the cells); read by the panel every frame.
    struct State {
        bool running = false;
        int done = 0, total = 0;      // backtests finished / planned
        int metric = 0;
        std::string label;            // "AAPL 1d 2y — SmaCrossover"
        // Current 1-D param sweep (for the live plot + progress line).
        std::string cur_param;
        int pass = 0, n_passes = 0;
        std::vector<double> xs, vals;
        // Result: best parameter set, its in-sample metric, applied or not.
        std::map<std::string, double> best;
        double best_metric = 0;
        bool has_best = false;
        bool applied = false;         // best written into the strategy's params
        double holdout_pct = 0;       // >0: holdout run follows the passes
        bool has_holdout = false;     // holdout_val is valid
        double holdout_val = 0;       // winner's metric on unseen data
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
    int metric_ = 0;
    double holdout_pct_ = 25.0;
    bool use_holdout_ = true;
};

// Auto-optimizer shape: passes over the params, steps per 1-D sweep. Pass 2+
// refines in a window this fraction of the full range, centered on the best.
constexpr int kSweepPasses = 2;
constexpr int kSweepSteps = 12;
constexpr double kSweepRefineWindow = 0.25;

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
