#pragma once

#include "engine/engine.h"
#include "market_data.h"   // QuoteBook (live bid/ask per symbol)

namespace tt::ui {

class PositionsPanel {
public:
    PositionsPanel(Engine& eng, QuoteBook& quotes) : eng_(eng), quotes_(quotes) {}
    void draw(bool* open);

private:
    Engine& eng_;
    QuoteBook& quotes_;
};

} // namespace tt::ui
