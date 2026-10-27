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

// One closed session's equity, oldest first - the input to roi_from_sessions.
struct EquityPoint {
    std::string date;          // local YYYY-MM-DD the session STARTED
    double initial_cash = 0;   // baseline equity, after set_baseline re-anchored
    double final_equity = 0;
};

// What a return figure is measured against.
struct RoiBasis {
    bool ok = false;                 // false = not enough usable history to say
    std::string from_date;           // the baseline session's date
    double baseline_equity = 0;
    double latest_equity = 0;
    int sessions = 0;                // closed sessions in the window
    // Month-to-date, taken from the month of the LATEST session rather than
    // from today's clock, so the figure and the period it names cannot disagree
    // when a report is emitted either side of midnight.
    bool mtd_ok = false;
    std::string mtd_from_date;
    double mtd_baseline_equity = 0;
};

// Pick the inception baseline and the month-to-date baseline from the closed
// session history (oldest first).
//
// THE TRAP THIS EXISTS TO AVOID. begin_session can only record a PLACEHOLDER
// baseline - on the TWS route the real account equity has not been replayed
// yet - and set_baseline re-anchors it once reconciliation finishes. Sessions
// recorded before that worked carry initial_cash = 100,000 against a real ~$1M
// account: 48 of this journal's 115 closed sessions, every one up to
// 2026-07-28. Measuring "return since inception" from one of those would report
// roughly +900%.
//
// So a baseline is only believed if it is within a factor of two of the latest
// equity. One rule, applied in one place - and RoiBasis::from_date is REPORTED,
// so a wrong pick is visible on the page rather than silently inflating a
// percentage. A heuristic that cannot be seen is the one that lies.
RoiBasis roi_from_sessions(const std::vector<EquityPoint>& closed);

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
    // Equity basis for the return figures on the daily summary. Reads every
    // CLOSED session (an open one has no final_equity) and hands them to
    // roi_from_sessions. Empty/unusable history comes back with ok = false,
    // never as a zero that would read as a flat account.
    RoiBasis roi_basis() const;
    std::vector<FillRow> fills(int64_t session_id) const;

private:
    sqlite3* db_ = nullptr;
    uint64_t rev_ = 1;
};

} // namespace tt::ui
