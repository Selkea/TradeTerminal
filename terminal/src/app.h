#pragma once

#include "market_data.h"
#include "net/ipc_client.h"
#include "panels/chart.h"
#include "panels/log_console.h"
#include "panels/watchlist.h"

#include "imgui.h"

#include <string>

namespace tt::ui {

// Owns the data stores, the sidecar connection, and the panels; draws one
// frame of UI inside the dockspace.
class App {
public:
    App(std::string python_cmd, std::string service_dir);
    ~App();

    void draw();

    // Whether an imgui.ini existed at startup; if not, a default dock layout
    // is built on the first frame.
    void set_had_ini(bool v) { had_ini_ = v; }
    LogConsole& log() { return log_; }

private:
    void draw_menu_bar();
    void setup_default_layout(ImGuiID dockspace_id);

    LogConsole log_;
    SeriesStore series_;
    QuoteBook quotes_;
    net::IpcClient ipc_;
    ChartPanel chart_;
    WatchlistPanel watchlist_;

    bool had_ini_ = false;
    bool layout_checked_ = false;
    bool show_chart_ = true;
    bool show_watchlist_ = true;
    bool show_log_ = true;
    bool show_imgui_demo_ = false;
    bool show_implot_demo_ = false;
};

} // namespace tt::ui
