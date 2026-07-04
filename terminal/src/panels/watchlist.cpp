#include "panels/watchlist.h"

#include "imgui.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace tt::ui {

void WatchlistPanel::resubscribe() {
    if (!ipc_.connected()) return;
    ipc_.subscribe_quotes(symbols_, 10);  // SUB_ACK retires the previous sub
    dirty_ = false;
}

void WatchlistPanel::draw(bool* open, const std::function<void(const std::string&)>& on_select) {
    const bool visible = ImGui::Begin("Watchlist", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    const uint64_t gen = ipc_.connection_generation();
    if (gen != seen_conn_gen_ && ipc_.connected()) {
        seen_conn_gen_ = gen;
        dirty_ = true;  // fresh connection: previous subscription is gone
    }
    if (dirty_) resubscribe();

    ImGui::SetNextItemWidth(90);
    const bool entered = ImGui::InputText("##add", input_, sizeof(input_),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || entered) && input_[0]) {
        std::string sym(input_);
        std::transform(sym.begin(), sym.end(), sym.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (std::find(symbols_.begin(), symbols_.end(), sym) == symbols_.end()) {
            symbols_.push_back(sym);
            dirty_ = true;
        }
        input_[0] = '\0';
    }

    if (ImGui::BeginTable("##watch", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 20.0f);
        ImGui::TableHeadersRow();

        int remove_at = -1;
        for (size_t i = 0; i < symbols_.size(); ++i) {
            const std::string& sym = symbols_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(sym.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap) && on_select)
                on_select(sym);

            Quote q;
            const bool has = quotes_.get(sym, q);
            ImGui::TableNextColumn();
            ImGui::Text(has ? "%.2f" : "--", q.price);
            ImGui::TableNextColumn();
            if (has && q.ts_ms > 0) {
                const std::time_t t = static_cast<std::time_t>(q.ts_ms / 1000);
                std::tm tm{};
                localtime_s(&tm, &t);
                ImGui::Text("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("x")) remove_at = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove_at >= 0) {
            symbols_.erase(symbols_.begin() + remove_at);
            dirty_ = true;
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace tt::ui
