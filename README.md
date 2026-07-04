# TradeTerminal

Desktop trading terminal for writing, backtesting, and paper-trading algorithmic
strategies in C++. Dear ImGui (docking) + ImPlot front-end, a low-latency
single-threaded engine core fed by lock-free SPSC rings, hot-compiled strategy
DLL plugins, and a Python sidecar for market data (Yahoo Finance, cached in
sqlite).

## Workflow

1. **Chart / Watchlist** — type a symbol, candles + delayed quotes appear
   (Python sidecar auto-starts; killing it is fine, it respawns).
2. **Strategy panel** — pick a `.cpp` from `strategies/`, hit **Build & Load**:
   the terminal compiles it with g++ and hot-loads the DLL. Edit the file and
   rebuild without restarting. Parameters declared by the strategy are editable.
3. **Backtest panel** — symbol/interval/range/cash, Run: full-speed replay
   (millions of events/sec) with modeled latency, slippage, and commissions.
   Bit-identical reruns; equity curve, drawdown, Sharpe, tick→order latency
   percentiles; Export CSV.
4. **Trade panel** — start a live paper session on the delayed feed: the same
   engine/exec-sim code path on a real-time clock. Manual orders, order
   blotter with cancel, live positions/PnL, pre-trade risk checks, and a
   **KILL SWITCH** (cancel all + flatten + halt).

Session state (watchlist, chart, cash settings) persists to
`%LOCALAPPDATA%\TradeTerminal\config.json`; logs rotate in
`...\TradeTerminal\logs\`. Live-broker and pro-feed integration points are
documented in `engine/include/engine/broker.h` and `feed.h`.

Debug/verification hooks (env vars): `TT_LOG_STDOUT=1` mirrors the log console
to stdout; `TT_AUTORUN_BACKTEST=1` runs an AAPL backtest at startup;
`TT_SIM_TICKS=1` feeds a synthetic 2 Hz walk to live sessions (markets closed);
`TT_AUTORUN_LIVE=1` runs a scripted live-session check; `TT_PYTHON`/`TT_GXX`
override tool paths.

## Layout

| Path | What |
|---|---|
| `sdk/` | Strategy SDK headers — POD events + `IStrategy` ABI (the DLL contract) |
| `engine/` | Static lib: rings, clocks, execution sim, portfolio, strategy host. No GUI deps. |
| `terminal/` | The ImGui app: panels, chart, IPC client, sidecar management |
| `strategies/` | User strategy `.cpp` files, compiled in-app to versioned DLLs |
| `dataservice/` | Python (stdlib-only) data sidecar: Yahoo OHLCV + cache + TCP server |
| `third_party/` | Vendored: imgui `v1.91.9b-docking`, implot `v0.17`, doctest (pinned pair — upgrade imgui+implot together only) |

## Build

Requires MSYS2 UCRT64 toolchain (`gcc`, `cmake`, `ninja`, `glfw` via pacman).

```
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --preset ucrt64-debug
```

Build output goes to `C:\dev\build\TradeTerminal\` and runtime data to
`%LOCALAPPDATA%\TradeTerminal\` — both deliberately outside this
OneDrive-synced source tree.

In VS Code: open this folder as its own workspace; CMake Tools picks up
`CMakePresets.json` automatically. F5 debugs `tt_terminal` under gdb.
