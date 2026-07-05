#pragma once

#include "engine/engine.h"

#include <functional>
#include <string>
#include <vector>

namespace tt::ui {

// Live paper-trading controls: start/stop a session, manual orders, kill
// switch, session status, and replay of captured sessions.
class TradePanel {
public:
    struct StartOpts {
        std::vector<std::string> symbols;
        double cash = 100'000.0;
        int bar_seconds = 60;
        bool alpaca_orders = false;   // route orders to the Alpaca paper API
        bool alpaca_data = false;     // real-time IEX ticks instead of delayed quotes
        bool record = true;           // capture ticks to a .ttk session file
        RiskLimits risk{};
    };
    using StartFn = std::function<void(const StartOpts&)>;
    using ReplayFn = std::function<void(const std::string& path)>;

    TradePanel(Engine& eng, std::string sessions_dir)
        : eng_(eng), sessions_dir_(std::move(sessions_dir)) {}
    // alpaca_available: credentials exist right now (signed-in account or env
    // vars) — evaluated per frame because sign-in/out happens at runtime.
    void draw(bool* open, const std::string& strategy_name, bool alpaca_available,
              const StartFn& start, const ReplayFn& replay);

    double cash() const { return cash_; }
    int bar_sec() const { return bar_sec_; }
    void restore(double cash, int bar_sec) {
        cash_ = cash;
        bar_sec_ = bar_sec;
    }
    // Risk limits persist across restarts (armed halts must stay armed).
    const RiskLimits& risk() const { return risk_; }
    double risk_dd_pct() const { return risk_dd_pct_; }
    void restore_risk(const RiskLimits& r, double dd_pct) {
        risk_ = r;
        risk_dd_pct_ = dd_pct;
    }

private:
    void scan_replay_files();

    Engine& eng_;
    std::string sessions_dir_;
    bool use_alpaca_ = false;        // route orders; deliberately not persisted
    bool use_alpaca_data_ = false;   // real-time IEX data for the session
    bool record_ticks_ = true;
    bool session_alpaca_ = false;    // what the running session was started with
    char input_[16] = "";
    std::vector<std::string> pending_symbols_ = {"AAPL"};
    double cash_ = 100'000.0;
    int bar_sec_ = 60;
    RiskLimits risk_{};
    double risk_dd_pct_ = 0.0;   // UI shows percent; RiskLimits stores fraction
    double manual_qty_ = 10.0;
    double manual_tp_ = 0.0, manual_sl_ = 0.0;
    int selected_symbol_idx_ = 0;
    std::vector<std::string> replay_files_;   // basenames, newest first
    int replay_idx_ = 0;
    bool replay_scanned_ = false;
};

} // namespace tt::ui
