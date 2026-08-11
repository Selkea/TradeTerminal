#include "panels/log_console.h"

#include "ui_hints.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <vector>

namespace tt::ui {

// Rotation on startup, to a name that CANNOT collide with an earlier rotation.
//
// WHY NOT "<path>.1". It was, until 0.20.0, and on 2026-08-11 that cost the
// investigation its evidence. A GUI launched while a dry run was in progress
// found an 11.25 MB optimizer.log, renamed it to optimizer.log.1 — over the
// previous optimizer.log.1 — and the 2026-08-10 baseline, the whole reason the
// build was being profiled, was gone. A fixed rotation name means rotation N+1
// silently destroys rotation N, and rotation happens on EVERY startup that finds
// an oversized file, including a mistaken second launch: the two events that
// most want the old log are exactly the two that delete it.
//
// A timestamp makes each rotation its own file, so a rotation can only ever
// ADD. kKeepRotations then prunes the oldest by name — which sorts
// chronologically, because the stamp is fixed-width and big-endian.
void LogConsole::set_log_file(std::string path) {
    std::lock_guard lock(mu_);
    file_path_ = std::move(path);
    std::error_code ec;
    namespace fs = std::filesystem;
    const fs::path p(file_path_);
    fs::create_directories(p.parent_path(), ec);
    if (!fs::exists(file_path_, ec) ||
        fs::file_size(file_path_, ec) <= static_cast<uintmax_t>(kRotateBytes))
        return;

    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[24];
    std::snprintf(stamp, sizeof stamp, ".%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    std::string rotated = file_path_ + stamp;
    // Two launches inside the same second is not a real scenario, but a rename
    // ONTO an existing file is precisely the data loss this function exists to
    // stop, so refuse rather than overwrite.
    if (fs::exists(rotated, ec)) return;
    fs::rename(file_path_, rotated, ec);
    if (ec) return;

    // Prune: keep the newest kKeepRotations. Collected by prefix and sorted by
    // name — the stamp above is zero-padded and most-significant-first, so
    // lexicographic order IS chronological order.
    const std::string base = p.filename().string() + ".";
    std::vector<std::string> old;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        // The stamp is exactly ".YYYYMMDD-HHMMSS": prefix plus 15 characters.
        // Length-checked so an unrelated sibling (optimizer.log.err, or the
        // pre-0.20.0 optimizer.log.1) is never a pruning candidate — this
        // function deletes files, and it may only ever delete its own.
        if (name.size() == base.size() + 15 && name.rfind(base, 0) == 0)
            old.push_back(e.path().string());
    }
    if (old.size() <= kKeepRotations) return;
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + kKeepRotations < old.size(); ++i)
        fs::remove(old[i], ec);
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

bool LogConsole::draw(const char* title, bool* open) {
    const bool visible = ImGui::Begin(title, open);
    tab_drag_hint();
    if (!visible) {
        ImGui::End();
        return false;
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
    return true;
}

} // namespace tt::ui
