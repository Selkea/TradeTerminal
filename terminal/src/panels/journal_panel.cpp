#include "panels/journal_panel.h"

#include "imgui.h"
#include "ui_hints.h"

namespace tt::ui {

namespace {
ImVec4 pnl_color(double v) {
    if (v > 0) return {0.25f, 0.85f, 0.45f, 1};
    if (v < 0) return {0.95f, 0.35f, 0.25f, 1};
    return {0.6f, 0.6f, 0.6f, 1};
}
} // namespace

void JournalPanel::refresh() {
    days_ = journal_.days();
    sessions_ = journal_.sessions();
    if (selected_session_) fills_ = journal_.fills(selected_session_);
    seen_rev_ = journal_.revision();
}

void JournalPanel::draw(bool* open) {
    const bool visible = ImGui::Begin("Journal", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }
    if (!journal_.ok()) {
        ImGui::TextDisabled("journal.db could not be opened — journaling disabled");
        ImGui::End();
        return;
    }
    if (seen_rev_ != journal_.revision()) refresh();

    if (ImGui::BeginTable("##days", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6))) {
        ImGui::TableSetupColumn("Day");
        ImGui::TableSetupColumn("Sessions");
        ImGui::TableSetupColumn("Fills");
        ImGui::TableSetupColumn("PnL");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& d : days_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.date.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.sessions);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.fills);
            ImGui::TableNextColumn();
            ImGui::TextColored(pnl_color(d.pnl), "%+.2f", d.pnl);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::BeginTable("##sessions", 6,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
        ImGui::TableSetupColumn("Started");
        ImGui::TableSetupColumn("Symbols");
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Fills");
        ImGui::TableSetupColumn("PnL");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& s : sessions_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(s.id));
            if (ImGui::Selectable(s.started.c_str(), selected_session_ == s.id,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selected_session_ = s.id;
                fills_ = journal_.fills(s.id);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.symbols.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.mode.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.fills);
            ImGui::TableNextColumn();
            if (s.open)
                ImGui::TextDisabled("(live)");
            else
                ImGui::TextColored(pnl_color(s.pnl), "%+.2f", s.pnl);
            ImGui::TableNextColumn();
            if (s.halted)
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "halted");
        }
        ImGui::EndTable();
    }

    if (selected_session_) {
        ImGui::Separator();
        ImGui::TextDisabled("Fills — session #%lld", static_cast<long long>(selected_session_));
        if (ImGui::BeginTable("##fills", 6,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Symbol");
            ImGui::TableSetupColumn("Side");
            ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("Price");
            ImGui::TableSetupColumn("Fee");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& f : fills_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.time.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.symbol.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(f.side == "buy" ? ImVec4(0.25f, 0.85f, 0.45f, 1)
                                                   : ImVec4(0.95f, 0.35f, 0.25f, 1),
                                   "%s", f.side.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", f.qty);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", f.price);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", f.fee);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace tt::ui
