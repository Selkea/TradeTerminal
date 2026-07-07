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

    // Per-symbol settings for a session (bar size, tick capture, sub-account).
    struct SymbolOpt {
        std::string symbol;
        int bar_seconds = 60;
        bool record = true;
        std::string account;   // IBKR sub-account id; "" = shared account/pool
    };
    struct StartOpts {
        std::vector<SymbolOpt> symbols;   // one entry per traded symbol
        double session_cash = 100'000.0;  // shared pool (simulator / single account)
        Broker broker = Broker::Sim;      // where orders route
        DataFeed data = DataFeed::Ibkr;   // where ticks come from
        RiskLimits risk{};                // session-wide
    };
    using StartFn = std::function<void(const StartOpts&)>;

    // Active account shown in the panel header (from the gateway/store).
    struct AccountInfo {
        std::string label;   // e.g. "claudiagosselin"; empty = not signed in
        int kind = 0;        // 0 unknown, 1 paper, 2 live
        bool readonly = false;
        // Tradeable (sub-)accounts under the login; >1 shows a per-symbol picker.
        std::vector<std::string> subaccounts;
    };

    explicit TradePanel(Engine& eng) : eng_(eng) {}
    // polygon_available / finnhub_available: a key for that vendor exists right
    // now (signed in or env var) — evaluated per frame because sign-in/out
    // happens at runtime. ibkr_ready: the IBKR gateway is connected, so orders
    // route to it; otherwise the local fill simulator is used.
    void draw(bool* open, const std::string& strategy_name, bool polygon_available,
              bool finnhub_available, bool ibkr_ready, const AccountInfo& account,
              const StartFn& start);

    // Persisted: the shared cash pool + per-symbol defaults for new cards.
    double cash() const { return session_cash_; }
    int bar_sec() const { return def_bar_sec_; }
    int data_idx() const { return data_idx_; }
    bool record() const { return def_record_; }
    void restore(double cash, int bar_sec, int data_idx, bool record) {
        session_cash_ = cash;
        def_bar_sec_ = bar_sec;
        data_idx_ = (data_idx >= 0 && data_idx <= 2) ? data_idx : 0;  // Ibkr/Poly/Finn
        def_record_ = record;
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
    int session_broker_ = 0;         // what the running session was started with
    char input_[16] = "";
    // One card per pending symbol: its bar size, capture flag, and (when the
    // account has sub-accounts) which one it trades in.
    struct SymRow {
        std::string symbol;
        int bar_sec = 60;
        bool record = true;
        int account_idx = 0;   // index into AccountInfo.subaccounts
    };
    std::vector<SymRow> pending_ = {{"AAPL", 60, true, 0}};
    // Shared-pool cash (simulator / single account) + per-symbol defaults.
    double session_cash_ = 100'000.0;
    int def_bar_sec_ = 60;
    bool def_record_ = true;
    RiskLimits risk_{};
    double risk_dd_pct_ = 0.0;   // UI shows percent; RiskLimits stores fraction
    double manual_qty_ = 10.0;
    double manual_tp_ = 0.0, manual_sl_ = 0.0;
    int selected_symbol_idx_ = 0;
};

} // namespace tt::ui
