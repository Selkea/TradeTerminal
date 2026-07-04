#include "panels/positions.h"

#include "imgui.h"
#include "ui_hints.h"

namespace tt::ui {

void PositionsPanel::draw(bool* open) {
    const bool visible = ImGui::Begin("Positions", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }
    const LiveSnapshot s = eng_.live_snapshot();
    if (!s.running && s.ticks == 0) {
        ImGui::TextDisabled("No live session.");
        ImGui::End();
        return;
    }

    ImGui::Text("equity %.2f   cash %.2f", s.equity, s.cash);
    ImGui::Separator();

    if (ImGui::BeginTable("##pos", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Avg");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Unrealized");
        ImGui::TableSetupColumn("Realized");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s.symbol.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", s.position.qty);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", s.position.avg_price);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", s.last_price);
        ImGui::TableNextColumn();
        const ImVec4 up(0.25f, 0.85f, 0.45f, 1), dn(0.9f, 0.35f, 0.3f, 1);
        ImGui::TextColored(s.position.unrealized_pnl >= 0 ? up : dn, "%+.2f",
                           s.position.unrealized_pnl);
        ImGui::TableNextColumn();
        ImGui::TextColored(s.position.realized_pnl >= 0 ? up : dn, "%+.2f",
                           s.position.realized_pnl);
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace tt::ui
