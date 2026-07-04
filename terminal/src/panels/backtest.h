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

    double cash() const { return cash_; }
    void set_cash(double c) { cash_ = c; }

private:
    void draw_results();
    void export_csv();

    Engine& eng_;
    char sym_[16] = "AAPL";
    int interval_idx_ = 2;
    int range_idx_ = 3;
    double cash_ = 100'000.0;
    BacktestResult res_;
    bool has_res_ = false;
    std::string last_export_;
};

} // namespace tt::ui
