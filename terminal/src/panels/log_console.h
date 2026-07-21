#pragma once

#include "imgui.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tt::ui {

// Scrolling log panel. add() is thread-safe so worker threads (IPC reader,
// engine via ui_ring later) can log directly; draw() runs on the UI thread.
class LogConsole {
public:
    // Also append to a file (5 MB rotation to <path>.1). Call once at startup.
    void set_log_file(std::string path);

    void add(std::string line);
    void draw(const char* title, bool* open);

    // Monotonic view of the ring for the diagnostics /logs endpoint: every line
    // carries an increasing id so a remote poller can request "everything since
    // N" and stitch an incremental tail together.
    struct Slice {
        uint64_t next_id = 0;          // cursor to send on the next poll
        bool dropped = false;          // lines between `since` and the ring were evicted
        std::vector<std::string> lines;
    };
    // Lines with id > since (thread-safe; callable from the server thread).
    // since == 0 (or a cursor left over from a previous process) returns a
    // bounded tail; a caught-up cursor returns no lines.
    Slice slice_since(uint64_t since);

private:
    std::mutex mu_;
    std::deque<std::string> lines_;
    uint64_t next_id_ = 1;             // id the next added line will receive
    std::string file_path_;
    ImGuiTextFilter filter_;
    bool auto_scroll_ = true;

    static constexpr size_t kMaxLines = 5000;
    static constexpr size_t kTailLines = 500;   // first-poll / resync tail
    static constexpr long kRotateBytes = 5 * 1024 * 1024;
};

} // namespace tt::ui
