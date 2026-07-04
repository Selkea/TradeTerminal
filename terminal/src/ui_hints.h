#pragma once

#include "imgui.h"

namespace tt::ui {

// Call immediately after ImGui::Begin(), before any early-out: the last item
// there is the window's dock tab (or title bar when floating), so the hint
// anchors to the tab even when the window itself is hidden behind another tab.
inline void tab_drag_hint() {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Click and drag to move or undock this panel.");
}

} // namespace tt::ui
