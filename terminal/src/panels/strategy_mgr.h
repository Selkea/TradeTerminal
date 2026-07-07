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
    // Compiler output, in its own dockable window.
    void draw_build_output(bool* open);
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

    // Declared parameters for a strategy, with each one's current value and
    // range — for panels (e.g. per-symbol Trade tabs) that edit params too.
    // Empty until the module is loaded (built-in is always available).
    struct ParamSpec {
        std::string name;
        double value, min, max;
    };
    std::vector<ParamSpec> param_specs(const std::string& key) const;

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

    // ---- session persistence (config.json) ----
    // Currently-loaded module keys, each's param values ("" = built-in), and a
    // restore that re-activates + rebuilds them and seeds their saved params.
    std::vector<std::string> loaded_keys() const;
    std::map<std::string, std::map<std::string, double>> all_param_values() const;
    void restore_state(const std::string& active, const std::vector<std::string>& loaded,
                       const std::map<std::string, std::map<std::string, double>>& params);

private:
    struct ParamValue {
        std::string name;
        double def, min, max, value;
    };

    void refresh_files();
    // One strategy's tab body: active/rebuild/unload controls + param editor.
    // mod is null for the built-in.
    void draw_strategy_tab(const std::string& key, const StrategyHost::ModuleView* mod);
    void start_build(const std::string& src, bool make_active);
    // Seed/merge the param editor values from a module's declared params.
    void adopt_params(const std::string& key);
    void console(std::string line);
    std::vector<ParamValue>* editor_params(const std::string& key);

    StrategyHost& host_;
    Engine& eng_;
    std::string dir_;

    std::vector<std::string> files_;   // .cpp basenames
    int build_sel_ = 0;                // index into files_ for the Build picker
    std::string want_tab_;             // tab-list button: key to select ("" built-in)
    bool want_tab_set_ = false;
    double next_refresh_s_ = 0.0;

    std::string active_key_;           // "" = built-in SMA
    std::deque<std::string> load_queue_;   // pending request_loads, built one at a time

    std::thread build_thread_;
    std::atomic<bool> building_{false};
    mutable std::mutex pending_mu_;
    std::string pending_dll_, pending_src_;
    bool pending_make_active_ = false;

    mutable std::mutex out_mu_;
    std::deque<std::string> output_;

    // Param values per strategy key ("" = built-in). UI thread only.
    std::map<std::string, std::vector<ParamValue>> param_vals_;
    // Saved values from the last session, applied by adopt_params on (re)load.
    std::map<std::string, std::map<std::string, double>> saved_params_;
};

} // namespace tt::ui
