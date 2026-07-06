# TradeTerminal

Windows desktop trading terminal for writing, backtesting, optimizing, and
trading algorithmic strategies in C++ through Interactive Brokers. Dear ImGui
(docking) + ImPlot front-end, a low-latency single-threaded engine core fed by
lock-free SPSC rings, hot-compiled strategy DLL plugins, and deterministic
(bit-identical) backtests and session replays.

## Market data & orders: IBKR

All market data, history, and orders flow through IBKR's **Client Portal
Gateway** — a small Java app extracted to `tools\clientportal.gw` (gitignored;
setup in [tools/README.md](tools/README.md)). The Sign In dialog auto-detects
it and offers a **Launch gateway** button; log in with your **paper trading
username** on the gateway's browser page and the terminal picks up the session
within seconds. Keep the repo on a clean local path — the gateway's web server
silently fails to bind its port when run from OneDrive or paths containing `+`.

Optional: a Polygon API key (Account menu → Sign In → Polygon, stored
encrypted with Windows DPAPI, or `POLYGON_API_KEY`) enables the Polygon
websocket as an alternative real-time feed.

## Workflow

1. **Sign in** — Account menu → Sign In → IBKR: Launch gateway, log in on its
   page, and the dialog turns green.
2. **Chart / Watchlist** — type a symbol; candles and quotes come from the
   gateway session.
3. **Strategy panel** — pick a `.cpp` from `strategies/`, hit **Build**: the
   terminal compiles it with g++ and hot-loads the DLL — edit and rebuild
   without restarting. Strategy-declared parameters are editable in the UI.
   Five strategies ship in the folder: SMA crossover (the annotated example),
   Donchian ATR trend following, Bollinger z-score reversion, opening range
   breakout, and RSI-2 pullback — see
   [strategies/README.md](strategies/README.md) for the catalog and the SDK
   contract.
4. **Backtest panel** — symbol/interval/range/cash, Run: full-speed replay
   (millions of events/sec) with modeled latency, slippage, and commissions.
   Bit-identical reruns; equity curve, drawdown, Sharpe, tick→order latency
   percentiles; CSV export.
5. **Optimizer panel** — parameter grid sweeps over the same deterministic
   backtest core.
6. **Trade panel** — live sessions on the real-time feed: **paper** (the
   backtest exec-sim on a real-time clock) or **broker** (orders routed to
   your IBKR paper account through the gateway). Manual orders, blotter with
   cancel, positions/PnL, trade journal, and session capture for deterministic
   replay. Risk rails: pre-trade size/price checks, daily-loss and drawdown
   halts, a stale-feed halt when holding a position blind, and a **KILL
   SWITCH** (cancel all + flatten + halt).

Session state (watchlist, chart, cash settings) persists to
`%LOCALAPPDATA%\TradeTerminal\config.json`; logs rotate in
`...\TradeTerminal\logs\`; captured sessions land in `...\TradeTerminal\sessions\`.

## Layout

| Path | What |
|---|---|
| `sdk/` | Strategy SDK headers — POD events + `IStrategy` ABI (the DLL contract) |
| `engine/` | Static lib: rings, clocks, execution sim, portfolio, IBKR feed/broker adapters, strategy host. No GUI deps. |
| `terminal/` | The ImGui app: panels, chart, gateway client, account store |
| `strategies/` | Strategy `.cpp` files, compiled in-app to hot-loaded DLLs |
| `tools/` | External runtime tools: the IBKR Client Portal Gateway lives here (gitignored) |
| `scripts/` | `vps_bootstrap.ps1` — one-shot trading-box setup (see below) |
| `third_party/` | Vendored: imgui `v1.91.9b-docking`, implot `v0.17`, doctest (pinned pair — upgrade imgui+implot together only) |

## Build

Requires the MSYS2 UCRT64 toolchain (`gcc`, `cmake`, `ninja`, `glfw`, `curl`,
`sqlite3` via pacman) and Java 8u192+ on PATH for the gateway.

```
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --preset ucrt64-debug
```

Build output goes to `C:\dev\build\TradeTerminal\` and runtime data to
`%LOCALAPPDATA%\TradeTerminal\` — both deliberately outside the source tree.

In VS Code: open this folder as its own workspace; CMake Tools picks up
`CMakePresets.json` automatically. F5 debugs `tt_terminal` under gdb.

## VPS deployment

`scripts\vps_bootstrap.ps1` (elevated PowerShell, fresh Windows box) installs
the toolchain, clones and builds the terminal, installs Java and the Client
Portal Gateway into the repo's `tools\` folder, applies trading-box tuning
(high-performance power plan, clock sync, core-pinning env vars), and runs a
connect-time check against the broker endpoints. Every step is idempotent.

## Debug / verification hooks (env vars)

`TT_LOG_STDOUT=1` mirrors the log console to stdout · `TT_AUTORUN_BACKTEST=1`
runs an AAPL backtest at startup · `TT_AUTORUN_SWEEP=1` runs a headless
optimizer grid · `TT_AUTORUN_LIVE=1` runs a scripted live-session check ·
`TT_SIM_TICKS=1` feeds a synthetic 2 Hz walk to live sessions (markets closed)
· `TT_IBKR_GATEWAY` overrides the gateway URL · `TT_POLYGON_WS` overrides the
Polygon websocket URL · `TT_PIN_ENGINE` / `TT_PIN_FEED` pin engine/feed
threads to cores · `TT_FEED_SPIN=1` busy-polls the feed thread ·
`TT_ALERT_WEBHOOK` sets the alert webhook · `TT_GXX` overrides the strategy
compiler path.

---

Trading involves substantial risk. This project is for research and paper
trading; nothing in it is investment advice, and the included strategies are
teaching examples, not products.
