#pragma once

#include "engine/engine.h"

#include <functional>
#include <string>
#include <vector>

namespace tt::ui {

// Live paper-trading controls: start/stop a session, manual orders, kill
// switch, and session status. (Session replay lives in the Backtest panel.)
class TradePanel {
public:
    enum class Broker : int { Sim = 0, Ibkr = 1 };
    enum class DataFeed : int { Ibkr = 0, Polygon = 1, Finnhub = 2 };

    struct StartOpts {
        std::vector<std::string> symbols;
        double cash = 100'000.0;
        int bar_seconds = 60;
        Broker broker = Broker::Sim;      // where orders route
        DataFeed data = DataFeed::Ibkr;   // where ticks come from
        bool record = true;                  // capture ticks to a .ttk session file
        RiskLimits risk{};
    };
    using StartFn = std::function<void(const StartOpts&)>;

    explicit TradePanel(Engine& eng) : eng_(eng) {}
    // polygon_available / finnhub_available: a key for that vendor exists right
    // now (signed in or env var) — evaluated per frame because sign-in/out
    // happens at runtime. ibkr_ready: the IBKR gateway is connected, so orders
    // route to it; otherwise the local fill simulator is used.
    void draw(bool* open, const std::string& strategy_name, bool polygon_available,
              bool finnhub_available, bool ibkr_ready, const StartFn& start);

    double cash() const { return cash_; }
    int bar_sec() const { return bar_sec_; }
    int data_idx() const { return data_idx_; }
    bool record() const { return record_ticks_; }
    void restore(double cash, int bar_sec, int data_idx, bool record) {
        cash_ = cash;
        bar_sec_ = bar_sec;
        data_idx_ = (data_idx >= 0 && data_idx <= 2) ? data_idx : 0;  // Ibkr/Poly/Finn
        record_ticks_ = record;
    }
    // Risk limits persist across restarts (armed halts must stay armed).
    const RiskLimits& risk() const { return risk_; }
    double risk_dd_pct() const { return risk_dd_pct_; }
    void restore_risk(const RiskLimits& r, double dd_pct) {
        risk_ = r;
        risk_dd_pct_ = dd_pct;
    }

private:
    Engine& eng_;
    int data_idx_ = 0;               // DataFeed enum (persisted)
    bool record_ticks_ = true;       // persisted
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
};

} // namespace tt::ui
