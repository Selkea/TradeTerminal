#include "panels/trade.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace tt::ui {

void TradePanel::draw(bool* open, const std::string& strategy_name, const StartFn& start) {
    if (!ImGui::Begin("Trade", open)) {
        ImGui::End();
        return;
    }

    const LiveSnapshot s = eng_.live_snapshot();

    if (!s.running) {
        ImGui::TextDisabled("Paper trading — %s", strategy_name.c_str());
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("symbol", sym_, sizeof(sym_), ImGuiInputTextFlags_CharsUppercase);
        ImGui::SetNextItemWidth(100);
        ImGui::InputDouble("cash", &cash_, 0, 0, "%.0f");
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("bar sec", &bar_sec_);
        bar_sec_ = std::clamp(bar_sec_, 1, 3600);

        ImGui::BeginDisabled(eng_.running());   // not while a backtest runs
        if (ImGui::Button("Start paper trading") && sym_[0] && start) {
            for (char* c = sym_; *c; ++c) *c = static_cast<char>(std::toupper(*c));
            start(sym_, cash_, bar_sec_);
        }
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    // ---- running session ----
    ImGui::Text("%s", s.symbol.c_str());
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
    ImGui::Text("last %.2f @ %s   ticks %llu%s", s.last_price, tbuf,
                static_cast<unsigned long long>(s.ticks),
                s.dropped_ticks ? " (drops!)" : "");
    ImGui::Text("equity %.2f   cash %.2f   pos %.0f", s.equity, s.cash, s.position.qty);

    ImGui::Separator();
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("##mqty", &manual_qty_, 0, 0, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button("Buy")) eng_.submit_manual(true, manual_qty_);
    ImGui::SameLine();
    if (ImGui::Button("Sell")) eng_.submit_manual(false, manual_qty_);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1));
    if (ImGui::Button("KILL SWITCH", ImVec2(-1, 0))) eng_.kill_switch();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Cancel all orders, flatten the position, halt the strategy");

    if (ImGui::Button("Stop session", ImVec2(-1, 0))) eng_.stop_live();

    ImGui::End();
}

} // namespace tt::ui
