#include "panels/log_console.h"

#include <chrono>
#include <ctime>

namespace tt::ui {

void LogConsole::add(std::string line) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::lock_guard lock(mu_);
    lines_.push_back(stamp + std::move(line));
    while (lines_.size() > kMaxLines) lines_.pop_front();
}

void LogConsole::draw(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear")) {
        std::lock_guard lock(mu_);
        lines_.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll_);
    ImGui::SameLine();
    filter_.Draw("Filter", 180.0f);

    ImGui::Separator();
    if (ImGui::BeginChild("##log_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard lock(mu_);
        for (const auto& line : lines_) {
            if (!filter_.PassFilter(line.c_str())) continue;
            ImGui::TextUnformatted(line.c_str());
        }
        if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace tt::ui
