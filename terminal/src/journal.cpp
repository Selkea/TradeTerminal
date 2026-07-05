#include "journal.h"

#include <sqlite3.h>

#include <ctime>

namespace tt::ui {

namespace {

// exec with no result rows; best-effort (journal must never block trading).
void exec(sqlite3* db, const char* sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS sessions(
  id INTEGER PRIMARY KEY,
  started_utc INTEGER NOT NULL,
  ended_utc INTEGER,
  symbols TEXT NOT NULL,
  mode TEXT NOT NULL,
  initial_cash REAL NOT NULL,
  final_equity REAL,
  halted INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS fills(
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL,
  ts_ns INTEGER NOT NULL,
  symbol TEXT NOT NULL,
  side TEXT NOT NULL,
  qty REAL NOT NULL,
  price REAL NOT NULL,
  fee REAL NOT NULL,
  order_id INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_fills_session ON fills(session_id);
)sql";

} // namespace

TradeJournal::~TradeJournal() {
    if (db_) sqlite3_close(db_);
}

bool TradeJournal::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_, "PRAGMA synchronous=NORMAL;");
    exec(db_, kSchema);
    return true;
}

int64_t TradeJournal::begin_session(const std::string& symbols, const std::string& mode,
                                    double initial_cash) {
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO sessions(started_utc,symbols,mode,initial_cash) "
                           "VALUES(?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_text(st, 2, symbols.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, mode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 4, initial_cash);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    ++rev_;
    return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

void TradeJournal::add_fill(int64_t session_id, int64_t ts_ns, const std::string& symbol,
                            bool buy, double qty, double price, double fee,
                            uint64_t order_id) {
    if (!db_ || session_id == 0) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO fills(session_id,ts_ns,symbol,side,qty,price,fee,"
                           "order_id) VALUES(?,?,?,?,?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int64(st, 2, ts_ns);
    sqlite3_bind_text(st, 3, symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, buy ? "buy" : "sell", -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, qty);
    sqlite3_bind_double(st, 6, price);
    sqlite3_bind_double(st, 7, fee);
    sqlite3_bind_int64(st, 8, static_cast<int64_t>(order_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    ++rev_;
}

void TradeJournal::end_session(int64_t session_id, double final_equity, bool halted) {
    if (!db_ || session_id == 0) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE sessions SET ended_utc=?, final_equity=?, halted=? "
                           "WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_double(st, 2, final_equity);
    sqlite3_bind_int(st, 3, halted ? 1 : 0);
    sqlite3_bind_int64(st, 4, session_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    ++rev_;
}

std::vector<TradeJournal::DayRow> TradeJournal::days(int limit) const {
    std::vector<DayRow> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT date(s.started_utc,'unixepoch','localtime') d, count(DISTINCT s.id), "
            "  (SELECT count(*) FROM fills f WHERE f.session_id IN "
            "     (SELECT id FROM sessions s2 WHERE "
            "      date(s2.started_utc,'unixepoch','localtime')=d)), "
            "  sum(COALESCE(s.final_equity - s.initial_cash, 0)) "
            "FROM sessions s GROUP BY d ORDER BY d DESC LIMIT ?",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DayRow r;
        r.date = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        r.sessions = sqlite3_column_int(st, 1);
        r.fills = sqlite3_column_int(st, 2);
        r.pnl = sqlite3_column_double(st, 3);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<TradeJournal::SessionRow> TradeJournal::sessions(int limit) const {
    std::vector<SessionRow> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT id, strftime('%Y-%m-%d %H:%M', started_utc,'unixepoch','localtime'), "
            "  symbols, mode, COALESCE(final_equity - initial_cash, 0), "
            "  (SELECT count(*) FROM fills f WHERE f.session_id = sessions.id), "
            "  halted, ended_utc IS NULL "
            "FROM sessions ORDER BY id DESC LIMIT ?",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        SessionRow r;
        r.id = sqlite3_column_int64(st, 0);
        r.started = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.symbols = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        r.mode = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        r.pnl = sqlite3_column_double(st, 4);
        r.fills = sqlite3_column_int(st, 5);
        r.halted = sqlite3_column_int(st, 6) != 0;
        r.open = sqlite3_column_int(st, 7) != 0;
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<TradeJournal::FillRow> TradeJournal::fills(int64_t session_id) const {
    std::vector<FillRow> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT strftime('%H:%M:%S', ts_ns/1000000000,'unixepoch','localtime'), "
            "  symbol, side, qty, price, fee FROM fills WHERE session_id=? ORDER BY id",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int64(st, 1, session_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        FillRow r;
        r.time = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        r.symbol = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.side = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        r.qty = sqlite3_column_double(st, 3);
        r.price = sqlite3_column_double(st, 4);
        r.fee = sqlite3_column_double(st, 5);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

} // namespace tt::ui
