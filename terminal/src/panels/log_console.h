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
    void add(std::string line);
    void draw(const char* title, bool* open);

private:
    std::mutex mu_;
    std::deque<std::string> lines_;
    ImGuiTextFilter filter_;
    bool auto_scroll_ = true;

    static constexpr size_t kMaxLines = 5000;
};

} // namespace tt::ui
