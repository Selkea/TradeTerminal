"""Yahoo Finance v8 chart endpoint fetcher.

Generalized from MyStocks.py (_fetch_json / fetch_quote): same endpoint and
browser User-Agent, extended from intraday closes to full OHLCV with
range/interval/period parameters. No API key required.
"""
import json
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://query1.finance.yahoo.com/v8/finance/chart/"

VALID_INTERVALS = {"1m", "2m", "5m", "15m", "30m", "60m", "90m", "1h", "1d", "5d", "1wk", "1mo"}

INTERVAL_SEC = {
    "1m": 60, "2m": 120, "5m": 300, "15m": 900, "30m": 1800,
    "60m": 3600, "90m": 5400, "1h": 3600, "1d": 86400, "5d": 5 * 86400,
    "1wk": 7 * 86400, "1mo": 30 * 86400,
}


class YahooError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _fetch_json(url: str) -> dict:
    # Yahoo rejects urllib's default User-Agent, so send a browser-ish one.
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _chart(symbol: str, params: dict) -> dict:
    url = BASE + urllib.parse.quote(symbol) + "?" + urllib.parse.urlencode(params)
    try:
        data = _fetch_json(url)
    except urllib.error.HTTPError as e:
        raise YahooError(f"http_{e.code}", f"{symbol}: HTTP {e.code}") from e
    except urllib.error.URLError as e:
        raise YahooError("network", f"{symbol}: {e.reason}") from e
    chart = data.get("chart") or {}
    err = chart.get("error")
    if err:
        raise YahooError(err.get("code", "yahoo"), err.get("description", str(err)))
    result = chart.get("result")
    if not result:
        raise YahooError("no_data", f"{symbol}: empty chart result")
    return result[0]


def fetch_ohlcv(symbol: str, interval: str = "1d", range_: str | None = None,
                period1: int | None = None, period2: int | None = None):
    """Returns (rows, meta): rows = [(ts_sec, o, h, l, c, v), ...] nulls dropped."""
    if interval not in VALID_INTERVALS:
        raise YahooError("bad_interval", f"unsupported interval {interval!r}")
    params = {"interval": interval}
    if range_:
        params["range"] = range_
    elif period1 is not None and period2 is not None:
        params["period1"] = int(period1)
        params["period2"] = int(period2)
    else:
        raise YahooError("bad_request", "need range or period1+period2")

    res = _chart(symbol, params)
    ts = res.get("timestamp") or []
    quote = (res.get("indicators", {}).get("quote") or [{}])[0]
    opens = quote.get("open") or []
    highs = quote.get("high") or []
    lows = quote.get("low") or []
    closes = quote.get("close") or []
    vols = quote.get("volume") or []

    rows = []
    for i, t in enumerate(ts):
        if i >= len(opens) or i >= len(highs) or i >= len(lows) or i >= len(closes):
            break
        o, h, l, c = opens[i], highs[i], lows[i], closes[i]
        if o is None or h is None or l is None or c is None:
            continue  # halted / partial row
        v = vols[i] if i < len(vols) and vols[i] is not None else 0.0
        rows.append((int(t), float(o), float(h), float(l), float(c), float(v)))
    return rows, (res.get("meta") or {})


def fetch_last_trade(symbol: str):
    """Cheap quote poll. Returns (ts_ms, price, day_volume)."""
    res = _chart(symbol, {"range": "1d", "interval": "1m"})
    meta = res.get("meta") or {}
    price = meta.get("regularMarketPrice")
    if price is None:
        raise YahooError("no_data", f"{symbol}: no regularMarketPrice")
    ts_ms = int(meta.get("regularMarketTime", 0)) * 1000
    day_vol = float(meta.get("regularMarketVolume") or 0.0)
    return ts_ms, float(price), day_vol
