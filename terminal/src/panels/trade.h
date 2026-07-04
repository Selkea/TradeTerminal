#pragma once

#include "engine/engine.h"

#include <functional>
#include <string>

namespace tt::ui {

// Live paper-trading controls: start/stop a session, manual orders, kill
// switch, session status.
class TradePanel {
public:
    // symbol, initial cash, bar aggregation seconds
    using StartFn = std::function<void(const std::string&, double, int)>;

    explicit TradePanel(Engine& eng) : eng_(eng) {}
    void draw(bool* open, const std::string& strategy_name, const StartFn& start);

private:
    Engine& eng_;
    char sym_[16] = "AAPL";
    double cash_ = 100'000.0;
    int bar_sec_ = 60;
    double manual_qty_ = 10.0;
};

} // namespace tt::ui
