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
    enum class Broker : int { Sim = 0, Alpaca = 1, Ibkr = 2 };
    enum class DataFeed : int { Delayed = 0, AlpacaIex = 1, Polygon = 2 };

    struct StartOpts {
        std::vector<std::string> symbols;
        double cash = 100'000.0;
        int bar_seconds = 60;
        Broker broker = Broker::Sim;         // where orders route
        DataFeed data = DataFeed::Delayed;   // where ticks come from
        bool record = true;                  // capture ticks to a .ttk session file
        RiskLimits risk{};
    };
    using StartFn = std::function<void(const StartOpts&)>;
    using ReplayFn = std::function<void(const std::string& path)>;

    TradePanel(Engine& eng, std::string sessions_dir)
        : eng_(eng), sessions_dir_(std::move(sessions_dir)) {}
    // *_available: credentials exist right now (signed-in account or env
    // vars) — evaluated per frame because sign-in/out happens at runtime.
    void draw(bool* open, const std::string& strategy_name, bool alpaca_available,
              bool polygon_available, const StartFn& start, const ReplayFn& replay);

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
    int broker_idx_ = 0;             // Broker enum; deliberately not persisted
    int data_idx_ = 0;               // DataFeed enum
    bool record_ticks_ = true;
    int session_broker_ = 0;         // what the running session was started with
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
