#pragma once
// Trade journal: every live session and fill persisted to SQLite
// (%LOCALAPPDATA%/TradeTerminal/journal.db). Session history should outlive
// the log rotation — "what did I trade last Tuesday and what did it make"
// must have an answer.
//
// UI thread only. Writes are per-fill / per-session (rare); WAL mode keeps
// them at microseconds. Never touches the engine thread.

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace tt::ui {

// WHICH BROKER ACTUALLY PLACED THESE ORDERS. This is the one field in the
// authoritative trade record that separates "this was real" from "this was a
// rehearsal", and it was wrong for every live session on the TWS route.
//
// The call site read `ibkr_ ? "ibkr" : "sim"` — a ternary written when IBKR's
// Client-Portal gateway was the only real route. When the TWS socket route was
// added it became the VPS's ONLY route, and `ibkr_` is null on it, so every
// session since 2026-07-08 was journalled as a SIMULATION while placing real
// orders against the paper account. 107 sessions, and the line immediately
// below the ternary already handled `tws_` explicitly, so the omission was
// invisible in review.
//
// Why it matters more than a label: journal.db is the only place a fill carries
// a symbol, so it is what a post-mortem reads. Reading mode='sim' after an
// incident says "no orders reached the market" — the exact wrong conclusion
// about the 2026-08-13 phantom position, and the exact wrong conclusion about
// any future one.
//
// Free function taking bools rather than another inline ternary so it can be
// tested: nothing in the suite constructs an App.
inline const char* live_broker_mode(bool has_tws, bool has_ibkr) {
    if (has_tws) return "tws";      // real orders, TWS socket route
    if (has_ibkr) return "ibkr";    // real orders, Client-Portal gateway route
    return "sim";                   // no broker: the internal simulator
}

class TradeJournal {
public:
    ~TradeJournal();

    bool open(const std::string& path);   // false = journaling disabled
    bool ok() const { return db_ != nullptr; }

    // Returns the session row id (0 on failure).
    int64_t begin_session(const std::string& symbols, const std::string& mode,
                          double initial_cash);
    // Re-anchor a session's PnL baseline (the `initial_cash` column) to the
    // real account equity once broker reconciliation completes. begin_session
    // can only capture a pre-reconciliation placeholder; on the TWS route that
    // is the ~100k notional, not the real ~1M account, which made per-session
    // PnL (final_equity - baseline) absurd. Call once, when live reports
    // reconciled==true.
    void set_baseline(int64_t session_id, double baseline_equity);
    void add_fill(int64_t session_id, int64_t ts_ns, const std::string& symbol,
                  bool buy, double qty, double price, double fee, uint64_t order_id);
    void end_session(int64_t session_id, double final_equity, bool halted);

    // Bumped on every write; the panel re-queries when it changes.
    uint64_t revision() const { return rev_; }

    struct DayRow {
        std::string date;   // local "YYYY-MM-DD"
        int sessions = 0;
        int fills = 0;
        double pnl = 0;
    };
    struct SessionRow {
        int64_t id = 0;
        std::string started;   // local "YYYY-MM-DD HH:MM"
        std::string symbols, mode;
        double pnl = 0;
        int fills = 0;
        bool halted = false, open = false;
    };
    struct FillRow {
        std::string time;   // local "HH:MM:SS"
        std::string symbol, side;
        double qty = 0, price = 0, fee = 0;
    };
    std::vector<DayRow> days(int limit = 60) const;
    std::vector<SessionRow> sessions(int limit = 100) const;
    std::vector<FillRow> fills(int64_t session_id) const;

private:
    sqlite3* db_ = nullptr;
    uint64_t rev_ = 1;
};

} // namespace tt::ui
