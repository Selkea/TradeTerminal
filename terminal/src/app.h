#pragma once

#include "panels/log_console.h"

#include "imgui.h"

namespace tt::ui {

// Owns the panels and draws one frame of UI inside the dockspace.
class App {
public:
    void draw();

    // Whether an imgui.ini existed at startup; if not, a default dock layout
    // is built on the first frame.
    void set_had_ini(bool v) { had_ini_ = v; }
    LogConsole& log() { return log_; }

private:
    void draw_menu_bar();
    void draw_chart_placeholder();
    void setup_default_layout(ImGuiID dockspace_id);

    LogConsole log_;
    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool show_chart_ = true;
    bool show_log_ = true;
    bool show_imgui_demo_ = false;
    bool show_implot_demo_ = false;
};

} // namespace tt::ui
