#pragma once

#include "imgui.h"

#include <deque>
#include <mutex>
#include <string>

namespace tt::ui {

// Scrolling log panel. add() is thread-safe so worker threads (IPC reader,
// engine via ui_ring later) can log directly; draw() runs on the UI thread.
class LogConsole {
public:
    // Also append to a file (5 MB rotation to <path>.1). Call once at startup.
    void set_log_file(std::string path);

    void add(std::string line);
    void draw(const char* title, bool* open);

private:
    std::mutex mu_;
    std::deque<std::string> lines_;
    std::string file_path_;
    ImGuiTextFilter filter_;
    bool auto_scroll_ = true;

    static constexpr size_t kMaxLines = 5000;
    static constexpr long kRotateBytes = 5 * 1024 * 1024;
};

} // namespace tt::ui
