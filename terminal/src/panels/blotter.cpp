#include "panels/blotter.h"

#include "imgui.h"
#include "ui_hints.h"

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
    const bool visible = ImGui::Begin("Blotter", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }
    const LiveSnapshot s = eng_.live_snapshot();

    // A LIQUIDATION THAT WAS ASKED FOR AND HAS NOT HAPPENED. broker->flatten()
    // returns void and yields no order id, so before 0.41.0 a flatten that
    // never reached the exchange looked exactly like one that filled: no rows
    // either way. 2026-08-06 was the first kind and cost $846 overnight.
    //
    // Above the table, in red, because it is the one thing here that is not
    // history — it is an expectation still outstanding.
    if (!s.flatten_pending.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.25f, 1));
        for (const LiveSnapshot::PendingFlatten& p : s.flatten_pending) {
            const std::time_t t =
                static_cast<std::time_t>(p.requested_ns / 1'000'000'000);
            std::tm tm{};
            localtime_s(&tm, &t);
            ImGui::Text("%s: flatten requested %02d:%02d:%02d for %s %+.0f - NOT YET FLAT",
                        p.why, tm.tm_hour, tm.tm_min, tm.tm_sec, p.symbol.c_str(),
                        p.qty);
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "The broker was asked to close this position and it is still open.\n"
            "A flatten yields no order id, so this line is the only thing that\n"
            "can tell you the request did not land.");
        ImGui::Separator();
    }

    if (s.orders.empty()) {
        ImGui::TextDisabled("No orders this session.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("##orders", 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Symbol");
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
            ImGui::TextUnformatted(o.symbol.c_str());
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
            else if (o.broker_originated)
                // Reconstructed from its fill: the engine never submitted this
                // id. The kill switch's and the EOD backstop's closes arrive
                // this way on the live route, and so would a foreign order.
                ImGui::TextUnformatted("mkt (broker)");
            else
                ImGui::TextUnformatted(o.manual ? "mkt (m)" : "mkt");
            if (o.broker_originated)
                ImGui::SetItemTooltip(
                    "Placed by the broker, not by this engine - a flatten, a\n"
                    "kill switch, or an order from another API client. It was\n"
                    "reconstructed from the fill, so its time is the FILL time\n"
                    "and no limit price is knowable.");
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
