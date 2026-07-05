#pragma once

#include "engine/engine.h"

#include <functional>
#include <string>
#include <vector>

namespace tt::ui {

// Live paper-trading controls: start/stop a session, manual orders, kill
// switch, session status.
class TradePanel {
public:
    // symbols, initial cash, bar aggregation seconds
    using StartFn = std::function<void(const std::vector<std::string>&, double, int)>;

    explicit TradePanel(Engine& eng) : eng_(eng) {}
    void draw(bool* open, const std::string& strategy_name, const StartFn& start);

    double cash() const { return cash_; }
    int bar_sec() const { return bar_sec_; }
    void restore(double cash, int bar_sec) {
        cash_ = cash;
        bar_sec_ = bar_sec;
    }

private:
    Engine& eng_;
    char input_[16] = "";
    std::vector<std::string> pending_symbols_ = {"AAPL"};
    double cash_ = 100'000.0;
    int bar_sec_ = 60;
    double manual_qty_ = 10.0;
    int selected_symbol_idx_ = 0;
};

} // namespace tt::ui
