#pragma once
// Session replay: re-run a captured .ttk tick recording through a strategy +
// the fill simulator, deterministically. Its own dockable window (moved out
// of the Backtest panel) with its own strategy pick, cash, and re-bar size.

#include "engine/engine.h"

#include <functional>
#include <string>
#include <vector>

namespace tt::ui {

class ReplayPanel {
public:
    // path, bar_seconds_override (>0 re-bars the recording, 0 = as recorded),
    // strategy key ("" = built-in SMA), starting cash for the simulator.
    using ReplayFn = std::function<void(const std::string& path, int bar_seconds_override,
                                        const std::string& strat_key, double cash)>;
    // Resolve a strategy key to its display name.
    using NameFn = std::function<std::string(const std::string& key)>;

    ReplayPanel(Engine& eng, std::string sessions_dir)
        : eng_(eng), sessions_dir_(std::move(sessions_dir)) {}

    // strat_keys: selectable strategies (loaded modules; built-in is added in
    // the UI), resolved to display names via `name`.
    void draw(bool* open, const std::vector<std::string>& strat_keys, const NameFn& name,
              const ReplayFn& replay);

    // Session persistence.
    double cash() const { return cash_; }
    const std::string& strategy() const { return strat_key_; }
    int bar_sec() const { return bar_sec_; }
    void restore(const std::string& strat, double cash, int bar_sec) {
        strat_key_ = strat;
        cash_ = cash;
        bar_sec_ = bar_sec < 0 ? 0 : bar_sec;
    }

private:
    void scan_files();

    Engine& eng_;
    std::string sessions_dir_;
    std::vector<std::string> files_;   // .ttk basenames, newest first
    int file_idx_ = 0;
    int bar_sec_ = 0;                  // re-bar size; 0 = as recorded
    bool scanned_ = false;
    std::string strat_key_;            // "" = built-in SMA
    double cash_ = 100'000.0;
};

} // namespace tt::ui
