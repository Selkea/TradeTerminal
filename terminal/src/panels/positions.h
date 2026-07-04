#pragma once

#include "engine/engine.h"

namespace tt::ui {

class PositionsPanel {
public:
    explicit PositionsPanel(Engine& eng) : eng_(eng) {}
    void draw(bool* open);

private:
    Engine& eng_;
};

} // namespace tt::ui
