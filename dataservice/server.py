"""asyncio TCP server: framed protocol on 127.0.0.1 (loopback only — no
firewall prompt). One client expected (the terminal), but nothing breaks with
several. Blocking Yahoo calls run in worker threads via asyncio.to_thread.
"""
import asyncio
import json
import sys
import time

import protocol as p
import yahoo
from cache import CandleCache

RANGE_SEC = {
    "1d": 86400, "5d": 5 * 86400, "1mo": 31 * 86400, "3mo": 93 * 86400,
    "6mo": 186 * 86400, "1y": 366 * 86400, "2y": 2 * 366 * 86400,
    "5y": 5 * 366 * 86400, "10y": 10 * 366 * 86400, "ytd": 366 * 86400,
    "max": 40 * 366 * 86400,
}

QUOTE_SPACING_S = 0.25   # politeness gap between per-symbol Yahoo calls
BACKOFF_MAX_S = 60.0


def log(msg: str) -> None:
    print(f"[dataservice] {msg}", flush=True)


class Session:
    """Per-connection state."""

    def __init__(self):
        self.subs: dict[int, asyncio.Task] = {}
        self.next_sub_id = 1
        self.write_lock = asyncio.Lock()


class DataService:
    def __init__(self, cache: CandleCache):
        self.cache = cache
        self.shutdown = asyncio.Event()

    # ---------- candles ----------

    def _get_candles_blocking(self, symbol: str, interval: str,
                              range_: str | None, period1, period2):
        """Cache-first fetch. Returns (rows, cached: bool)."""
        now = int(time.time())
        if range_:
            ts_from = now - RANGE_SEC.get(range_, RANGE_SEC["1y"])
            ts_to = now + 60
        else:
            ts_from, ts_to = int(period1), int(period2)

        ivl_sec = yahoo.INTERVAL_SEC.get(interval, 86400)
        info = self.cache.fetch_info(symbol, interval)
        if info:
            fetched_at, cover_from, cover_to = info
            covered = cover_from <= ts_from
            # Historical window fully inside what we've already fetched: no
            # point re-asking Yahoo, the past doesn't change.
            all_past = covered and cover_to >= ts_to and ts_to < now - 2 * ivl_sec
            # Otherwise fresh means we hit Yahoo recently for this series.
            fresh = covered and (now - fetched_at) < max(60, ivl_sec)
            if all_past or fresh:
                rows = self.cache.get(symbol, interval, ts_from, ts_to)
                if rows:
                    return rows, True

        try:
            fetched, _meta = yahoo.fetch_ohlcv(
                symbol, interval, range_=range_, period1=period1, period2=period2)
            self.cache.put(symbol, interval, fetched)
            self.cache.record_fetch(symbol, interval, now, ts_from, min(ts_to, now))
        except yahoo.YahooError:
            # Offline / rate-limited: serve stale cache if we have any.
            rows = self.cache.get(symbol, interval, ts_from, ts_to)
            if rows:
                return rows, True
            raise
        rows = self.cache.get(symbol, interval, ts_from, ts_to)
        return rows, False

    async def handle_req_candles(self, session, writer, req: dict):
        rid = req.get("id", 0)
        symbol = str(req.get("symbol", "")).upper().strip()
        interval = req.get("interval", "1d")
        range_ = req.get("range")
        period1, period2 = req.get("period1"), req.get("period2")
        if not symbol:
            await self.send(session, writer, p.jframe(
                p.ERROR, {"id": rid, "code": "bad_request", "message": "missing symbol"}))
            return
        try:
            rows, cached = await asyncio.to_thread(
                self._get_candles_blocking, symbol, interval, range_, period1, period2)
        except yahoo.YahooError as e:
            log(f"candles {symbol} {interval}: ERROR {e.code}: {e}")
            await self.send(session, writer, p.jframe(
                p.ERROR, {"id": rid, "code": e.code, "message": str(e)}))
            return
        meta = {"id": rid, "symbol": symbol, "interval": interval, "cached": cached}
        log(f"candles {symbol} {interval}: {len(rows)} rows (cached={cached})")
        await self.send(session, writer, p.candles_response(meta, rows))

    # ---------- quote subscriptions ----------

    async def poll_quotes(self, session, writer, sub_id: int,
                          symbols: list[str], poll_s: float):
        last_sent: dict[str, tuple] = {}
        backoff = 0.0
        while not self.shutdown.is_set():
            batch = bytearray()
            for idx, sym in enumerate(symbols):
                try:
                    ts_ms, price, day_vol = await asyncio.to_thread(
                        yahoo.fetch_last_trade, sym)
                    backoff = 0.0
                except yahoo.YahooError as e:
                    if e.code == "http_429":
                        backoff = min(max(backoff * 2, 2.0), BACKOFF_MAX_S)
                        log(f"quotes: 429 for {sym}, backing off {backoff:.0f}s")
                        await asyncio.sleep(backoff)
                    continue
                if last_sent.get(sym) != (ts_ms, price):
                    last_sent[sym] = (ts_ms, price)
                    batch += p.TICK.pack(sub_id, idx, ts_ms, price, day_vol)
                await asyncio.sleep(QUOTE_SPACING_S)
            if batch:
                await self.send(session, writer, p.frame(p.TICKS, bytes(batch)))
            await asyncio.sleep(poll_s)

    async def handle_sub(self, session, writer, req: dict):
        rid = req.get("id", 0)
        symbols = [str(s).upper().strip() for s in req.get("symbols", []) if str(s).strip()]
        poll_s = max(2.0, float(req.get("poll_s", 15)))
        sub_id = session.next_sub_id
        session.next_sub_id += 1
        if symbols:
            task = asyncio.create_task(
                self.poll_quotes(session, writer, sub_id, symbols, poll_s))
            session.subs[sub_id] = task
        log(f"sub {sub_id}: {symbols} every {poll_s}s")
        await self.send(session, writer, p.jframe(p.SUB_ACK, {
            "id": rid, "sub": sub_id,
            "symbols": {s: i for i, s in enumerate(symbols)}}))

    # ---------- plumbing ----------

    @staticmethod
    async def send(session, writer, data: bytes):
        async with session.write_lock:
            writer.write(data)
            await writer.drain()

    async def handle_client(self, reader, writer):
        peer = writer.get_extra_info("peername")
        log(f"client connected: {peer}")
        session = Session()
        await self.send(session, writer, p.jframe(
            p.HELLO, {"proto": 1, "impl": "dataservice/0.1"}))
        try:
            while True:
                msg_type, payload = await p.read_frame(reader)
                if msg_type == p.HELLO:
                    pass
                elif msg_type == p.REQ_CANDLES:
                    asyncio.create_task(self.handle_req_candles(
                        session, writer, json.loads(payload)))
                elif msg_type == p.SUB_QUOTES:
                    await self.handle_sub(session, writer, json.loads(payload))
                elif msg_type == p.UNSUB:
                    sub = json.loads(payload).get("sub")
                    task = session.subs.pop(sub, None)
                    if task:
                        task.cancel()
                    log(f"unsub {sub}")
                elif msg_type == p.PING:
                    await self.send(session, writer, p.frame(p.PONG))
                elif msg_type == p.SHUTDOWN:
                    log("shutdown requested")
                    self.shutdown.set()
                    break
                else:
                    log(f"unknown msg type 0x{msg_type:02x}")
        except (asyncio.IncompleteReadError, ConnectionError):
            log(f"client disconnected: {peer}")
        finally:
            for task in session.subs.values():
                task.cancel()
            writer.close()


async def serve(port: int, db_path: str | None = None) -> None:
    cache = CandleCache(db_path)
    service = DataService(cache)
    server = await asyncio.start_server(service.handle_client, "127.0.0.1", port)
    actual_port = server.sockets[0].getsockname()[1]
    # The terminal parses this line from stdout to find us.
    print(f"TT_PORT={actual_port}", flush=True)
    log(f"listening on 127.0.0.1:{actual_port}")
    async with server:
        await service.shutdown.wait()
    cache.close()
    log("bye")
