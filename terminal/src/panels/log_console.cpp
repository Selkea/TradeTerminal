#include "panels/log_console.h"

#include "ui_hints.h"

#include <algorithm>
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
    ++next_id_;   // lines_.back() has id (next_id_ - 1)
    if (!file_path_.empty()) {
        if (FILE* f = std::fopen(file_path_.c_str(), "a")) {
            std::fputs(lines_.back().c_str(), f);
            std::fputc('\n', f);
            std::fclose(f);
        }
    }
    while (lines_.size() > kMaxLines) lines_.pop_front();
}

LogConsole::Slice LogConsole::slice_since(uint64_t since) {
    Slice out;
    std::lock_guard lock(mu_);
    // The cursor is the highest id handed out so far (0 when empty), NOT next_id_:
    // the client polls back with it as `since`, and we return id > since, so the
    // cursor must be an id that actually exists or the newest line is skipped.
    const uint64_t last_id = next_id_ - 1;
    out.next_id = last_id;
    const uint64_t first_id = next_id_ - lines_.size();   // id of lines_.front()
    uint64_t start;                                       // first id to return
    if (since == 0 || since > last_id) {
        // First poll, or a cursor from a previous process (ids reset on restart):
        // hand back a bounded tail rather than the whole ring.
        if (since > last_id) out.dropped = true;
        start = next_id_ - std::min(lines_.size(), kTailLines);
    } else {
        start = since + 1;
    }
    if (start < first_id) {   // requested window already fell off the ring
        out.dropped = true;
        start = first_id;
    }
    for (size_t i = static_cast<size_t>(start - first_id); i < lines_.size(); ++i)
        out.lines.push_back(lines_[i]);
    return out;
}

void LogConsole::draw(const char* title, bool* open) {
    const bool visible = ImGui::Begin(title, open);
    tab_drag_hint();
    if (!visible) {
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
