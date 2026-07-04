#pragma once

#include "engine/engine.h"

namespace tt::ui {

// Order blotter: every order of the live session, newest first, with manual
// cancel for working orders.
class BlotterPanel {
public:
    explicit BlotterPanel(Engine& eng) : eng_(eng) {}
    void draw(bool* open);

private:
    Engine& eng_;
};

} // namespace tt::ui
