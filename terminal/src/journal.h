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

class TradeJournal {
public:
    ~TradeJournal();

    bool open(const std::string& path);   // false = journaling disabled
    bool ok() const { return db_ != nullptr; }

    // Returns the session row id (0 on failure).
    int64_t begin_session(const std::string& symbols, const std::string& mode,
                          double initial_cash);
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
