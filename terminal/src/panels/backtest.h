#pragma once

#include "engine/engine.h"

#include <functional>
#include <map>
#include <string>

namespace tt::ui {

class BacktestPanel {
public:
    // symbol, interval, range, initial cash — strategy + params come from
    // the Strategy Manager.
    using RunFn = std::function<void(const std::string&, const std::string&,
                                     const std::string&, double)>;

    explicit BacktestPanel(Engine& eng) : eng_(eng) {}
    void draw(bool* open, const std::string& strategy_name, const RunFn& run);

private:
    void draw_results();

    Engine& eng_;
    char sym_[16] = "AAPL";
    int interval_idx_ = 2;
    int range_idx_ = 3;
    double cash_ = 100'000.0;
    BacktestResult res_;
    bool has_res_ = false;
};

} // namespace tt::ui
