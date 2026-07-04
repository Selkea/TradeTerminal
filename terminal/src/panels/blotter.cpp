#include "panels/blotter.h"

#include "imgui.h"

#include <ctime>

namespace tt::ui {

namespace {
const char* status_str(OrderStatus s) {
    switch (s) {
    case OrderStatus::Working: return "working";
    case OrderStatus::Filled: return "filled";
    case OrderStatus::Cancelled: return "cancelled";
    case OrderStatus::Rejected: return "REJECTED";
    }
    return "?";
}
} // namespace

void BlotterPanel::draw(bool* open) {
    if (!ImGui::Begin("Blotter", open)) {
        ImGui::End();
        return;
    }
    const LiveSnapshot s = eng_.live_snapshot();
    if (s.orders.empty()) {
        ImGui::TextDisabled("No orders this session.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("##orders", 8,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Fill");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableHeadersRow();

        for (auto it = s.orders.rbegin(); it != s.orders.rend(); ++it) {
            const OrderRecord& o = *it;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(o.id));
            ImGui::TableNextColumn();
            const std::time_t t = static_cast<std::time_t>(o.ts_ns / 1'000'000'000);
            std::tm tm{};
            localtime_s(&tm, &t);
            ImGui::Text("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            ImGui::TableNextColumn();
            const bool buy = o.side == static_cast<uint8_t>(Side::Buy);
            ImGui::TextColored(buy ? ImVec4(0.25f, 0.85f, 0.45f, 1)
                                   : ImVec4(0.9f, 0.35f, 0.3f, 1),
                               buy ? "BUY" : "SELL");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", o.qty);
            ImGui::TableNextColumn();
            if (o.type == static_cast<uint8_t>(OrdType::Limit))
                ImGui::Text("lim %.2f", o.limit_price);
            else
                ImGui::TextUnformatted(o.manual ? "mkt (m)" : "mkt");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(status_str(o.status));
            ImGui::TableNextColumn();
            if (o.status == OrderStatus::Filled)
                ImGui::Text("%.2f", o.fill_price);
            else
                ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (o.status == OrderStatus::Working) {
                ImGui::PushID(static_cast<int>(o.id));
                if (ImGui::SmallButton("Cancel")) eng_.request_cancel(o.id);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace tt::ui
