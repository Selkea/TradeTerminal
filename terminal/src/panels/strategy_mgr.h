#pragma once

#include "engine/engine.h"
#include "engine/strategy_host.h"
#include "tt/strategy_api.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt::ui {

// Lists strategies/*.cpp, builds them with g++ (worker thread, output
// streamed into an in-panel console), hot-loads DLLs side by side, tracks
// which strategy is "active" (used by live/replay/trade), and edits each
// loaded module's parameters. Instances are created per run by the App via
// the host; this panel never hands out IStrategy pointers.
class StrategyManagerPanel {
public:
    StrategyManagerPanel(StrategyHost& host, Engine& eng, std::string strategies_dir);
    ~StrategyManagerPanel();

    void draw(bool* open);
    // Advances deferred work (queued builds, loading freshly built DLLs).
    // Called by the app every frame so progress doesn't depend on this
    // panel being visible.
    void pump();

    // Strategy the live/replay/trade side runs ("" = built-in SMA).
    const std::string& active_key() const { return active_key_; }
    std::string active_name() const { return display_name(active_key_); }
    std::string display_name(const std::string& key) const;
    // UI-edited parameter values for a strategy (defaults when untouched).
    std::map<std::string, double> param_values(const std::string& key) const;

    // ---- strategy selection for other panels (Backtest dropdown) ----
    // .cpp basenames in the strategies dir.
    const std::vector<std::string>& sources() const { return files_; }
    // Module loaded and its source unchanged since the build?
    bool loaded_fresh(const std::string& key) const;
    // Ensure `key` has a fresh module: builds + loads if absent or stale
    // (asynchronous; poll loaded_fresh()/load_pending()). "" is a no-op.
    void request_load(const std::string& key);
    // True while a build runs or a built DLL awaits loading.
    bool load_pending() const;

private:
    struct ParamValue {
        std::string name;
        double def, min, max, value;
    };

    void refresh_files();
    void start_build(const std::string& src, bool make_active);
    // Seed/merge the param editor values from a module's declared params.
    void adopt_params(const std::string& key);
    void console(std::string line);
    std::vector<ParamValue>* editor_params(const std::string& key);

    StrategyHost& host_;
    Engine& eng_;
    std::string dir_;

    std::vector<std::string> files_;   // .cpp basenames
    int selected_ = 0;                 // 0 = built-in, 1.. = files_[i-1]
    double next_refresh_s_ = 0.0;

    std::string active_key_;           // "" = built-in SMA
    std::string want_load_;            // deferred request_load while building

    std::thread build_thread_;
    std::atomic<bool> building_{false};
    mutable std::mutex pending_mu_;
    std::string pending_dll_, pending_src_;
    bool pending_make_active_ = false;

    mutable std::mutex out_mu_;
    std::deque<std::string> output_;

    // Param values per strategy key ("" = built-in). UI thread only.
    std::map<std::string, std::vector<ParamValue>> param_vals_;
};

} // namespace tt::ui
