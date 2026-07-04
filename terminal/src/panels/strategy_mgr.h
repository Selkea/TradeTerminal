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
// streamed into an in-panel console), hot-loads the DLL when the engine is
// idle, and edits the loaded strategy's declared parameters.
class StrategyManagerPanel {
public:
    StrategyManagerPanel(StrategyHost& host, Engine& eng, std::string strategies_dir);
    ~StrategyManagerPanel();

    void draw(bool* open);

    // The strategy backtests run with: the loaded DLL, or the built-in.
    IStrategy* active_strategy(IStrategy& builtin);
    std::string active_name() const;
    std::map<std::string, double> param_values() const;

private:
    struct ParamValue {
        std::string name;
        double def, min, max, value;
    };

    void refresh_files();
    void start_build(const std::string& src);
    void use_builtin_params();
    void adopt_loaded_params();
    void console(std::string line);

    StrategyHost& host_;
    Engine& eng_;
    std::string dir_;

    std::vector<std::string> files_;   // .cpp basenames
    int selected_ = 0;
    double next_refresh_s_ = 0.0;

    std::thread build_thread_;
    std::atomic<bool> building_{false};
    std::mutex pending_mu_;
    std::string pending_dll_, pending_src_;

    mutable std::mutex out_mu_;
    std::deque<std::string> output_;

    std::vector<ParamValue> params_;   // guarded by UI-thread-only access
};

} // namespace tt::ui
