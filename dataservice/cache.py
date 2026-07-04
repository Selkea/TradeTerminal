"""sqlite3 candle cache at %LOCALAPPDATA%/TradeTerminal/cache/market.db.

Fetches run on worker threads (asyncio.to_thread), so all DB access is
serialized behind one lock.
"""
import os
import sqlite3
import threading


def default_db_path() -> str:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "TradeTerminal", "cache", "market.db")


class CandleCache:
    def __init__(self, path: str | None = None):
        path = path or default_db_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self._lock = threading.Lock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.execute(
            "CREATE TABLE IF NOT EXISTS candles ("
            " symbol TEXT NOT NULL, interval TEXT NOT NULL, ts INTEGER NOT NULL,"
            " o REAL, h REAL, l REAL, c REAL, v REAL,"
            " PRIMARY KEY (symbol, interval, ts))")
        # One row per (symbol, interval): when we last hit Yahoo successfully
        # and what request window the cached rows cover. Freshness decisions
        # key off fetched_at, NOT candle timestamps (which stop advancing
        # whenever the market closes).
        self._db.execute(
            "CREATE TABLE IF NOT EXISTS fetches ("
            " symbol TEXT NOT NULL, interval TEXT NOT NULL,"
            " fetched_at INTEGER NOT NULL, cover_from INTEGER NOT NULL,"
            " cover_to INTEGER NOT NULL,"
            " PRIMARY KEY (symbol, interval))")
        self._db.commit()

    def put(self, symbol: str, interval: str, rows) -> None:
        if not rows:
            return
        with self._lock:
            self._db.executemany(
                "INSERT OR REPLACE INTO candles VALUES (?,?,?,?,?,?,?,?)",
                [(symbol, interval, *r) for r in rows])
            self._db.commit()

    def get(self, symbol: str, interval: str, ts_from: int = 0, ts_to: int = 2**62):
        with self._lock:
            cur = self._db.execute(
                "SELECT ts,o,h,l,c,v FROM candles"
                " WHERE symbol=? AND interval=? AND ts>=? AND ts<=? ORDER BY ts",
                (symbol, interval, ts_from, ts_to))
            return cur.fetchall()

    def last_ts(self, symbol: str, interval: str) -> int | None:
        with self._lock:
            row = self._db.execute(
                "SELECT MAX(ts) FROM candles WHERE symbol=? AND interval=?",
                (symbol, interval)).fetchone()
            return row[0] if row and row[0] is not None else None

    def record_fetch(self, symbol: str, interval: str, fetched_at: int,
                     ts_from: int, ts_to: int) -> None:
        with self._lock:
            row = self._db.execute(
                "SELECT cover_from, cover_to FROM fetches WHERE symbol=? AND interval=?",
                (symbol, interval)).fetchone()
            if row:
                ts_from = min(ts_from, row[0])
                ts_to = max(ts_to, row[1])
            self._db.execute(
                "INSERT OR REPLACE INTO fetches VALUES (?,?,?,?,?)",
                (symbol, interval, fetched_at, ts_from, ts_to))
            self._db.commit()

    def fetch_info(self, symbol: str, interval: str):
        """Returns (fetched_at, cover_from, cover_to) or None."""
        with self._lock:
            return self._db.execute(
                "SELECT fetched_at, cover_from, cover_to FROM fetches"
                " WHERE symbol=? AND interval=?", (symbol, interval)).fetchone()

    def close(self) -> None:
        with self._lock:
            self._db.commit()
            self._db.close()
