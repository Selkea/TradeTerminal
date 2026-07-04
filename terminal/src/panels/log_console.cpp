#include "panels/log_console.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace tt::ui {

void LogConsole::set_log_file(std::string path) {
    std::lock_guard lock(mu_);
    file_path_ = std::move(path);
    std::error_code ec;
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(file_path_).parent_path(), ec);
    if (fs::exists(file_path_, ec) &&
        fs::file_size(file_path_, ec) > static_cast<uintmax_t>(kRotateBytes)) {
        fs::rename(file_path_, file_path_ + ".1", ec);  // simple 1-deep rotation
    }
}

void LogConsole::add(std::string line) {
    // TT_LOG_STDOUT=1 mirrors the console to stdout (headless verification).
    static const bool echo = std::getenv("TT_LOG_STDOUT") != nullptr;
    if (echo) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::lock_guard lock(mu_);
    lines_.push_back(stamp + std::move(line));
    if (!file_path_.empty()) {
        if (FILE* f = std::fopen(file_path_.c_str(), "a")) {
            std::fputs(lines_.back().c_str(), f);
            std::fputc('\n', f);
            std::fclose(f);
        }
    }
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
