#include "panels/trade.h"

#include "imgui.h"
#include "ui_hints.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace tt::ui {

void TradePanel::draw(bool* open, const std::string& strategy_name, const StartFn& start) {
    const bool visible = ImGui::Begin("Trade", open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return;
    }

    const LiveSnapshot s = eng_.live_snapshot();

    if (!s.running) {
        ImGui::TextDisabled("Paper trading — %s", strategy_name.c_str());

        ImGui::SetNextItemWidth(80);
        const bool entered = ImGui::InputText("##add_sym", input_, sizeof(input_),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        if ((ImGui::Button("Add") || entered) && input_[0]) {
            std::string sym(input_);
            std::transform(sym.begin(), sym.end(), sym.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (std::find(pending_symbols_.begin(), pending_symbols_.end(), sym) ==
                pending_symbols_.end())
                pending_symbols_.push_back(sym);
            input_[0] = '\0';
        }
        int remove_at = -1;
        for (size_t i = 0; i < pending_symbols_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(pending_symbols_[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_at = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove_at >= 0) pending_symbols_.erase(pending_symbols_.begin() + remove_at);

        ImGui::SetNextItemWidth(100);
        ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("bar sec", &bar_sec_);
        bar_sec_ = std::clamp(bar_sec_, 1, 3600);

        ImGui::BeginDisabled(eng_.running());   // not while a backtest runs
        if (ImGui::Button("Start paper trading") && !pending_symbols_.empty() && start)
            start(pending_symbols_, cash_, bar_sec_);
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    // ---- running session ----
    std::string sym_list;
    for (const SymbolState& sym : s.symbols)
        sym_list += (sym_list.empty() ? "" : ", ") + sym.symbol;
    ImGui::Text("%s", sym_list.c_str());
    ImGui::SameLine();
    if (s.halted)
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "HALTED");
    else
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "LIVE (paper)");

    char tbuf[16] = "--";
    if (s.last_tick_ts_ms > 0) {
        const std::time_t t = static_cast<std::time_t>(s.last_tick_ts_ms / 1000);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    ImGui::Text("ticks %llu%s   last tick @ %s", static_cast<unsigned long long>(s.ticks),
                s.dropped_ticks ? " (drops!)" : "", tbuf);
    ImGui::Text("equity %.2f   cash %.2f", s.equity, s.cash);
    for (const SymbolState& sym : s.symbols)
        ImGui::Text("  %s   last %.2f   pos %.0f", sym.symbol.c_str(), sym.last_price,
                    sym.position.qty);

    ImGui::Separator();
    if (selected_symbol_idx_ >= static_cast<int>(s.symbols.size())) selected_symbol_idx_ = 0;
    if (s.symbols.size() > 1) {
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##manual_sym", s.symbols[selected_symbol_idx_].symbol.c_str())) {
            for (int i = 0; i < static_cast<int>(s.symbols.size()); ++i)
                if (ImGui::Selectable(s.symbols[i].symbol.c_str(), i == selected_symbol_idx_))
                    selected_symbol_idx_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("##mqty", &manual_qty_, 0, 0, "%.0f");
    ImGui::SameLine();
    const uint32_t manual_sid = static_cast<uint32_t>(selected_symbol_idx_ + 1);
    if (ImGui::Button("Buy")) eng_.submit_manual(manual_sid, true, manual_qty_);
    ImGui::SameLine();
    if (ImGui::Button("Sell")) eng_.submit_manual(manual_sid, false, manual_qty_);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1));
    if (ImGui::Button("KILL SWITCH", ImVec2(-1, 0))) eng_.kill_switch();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Cancel all orders, flatten the position, halt the strategy");

    if (ImGui::Button("Stop session", ImVec2(-1, 0))) eng_.stop_live();

    ImGui::End();
}

} // namespace tt::ui
