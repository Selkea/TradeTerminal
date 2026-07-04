# TradeTerminal

Desktop trading terminal for writing, backtesting, and paper-trading algorithmic
strategies in C++. Dear ImGui (docking) + ImPlot front-end, a low-latency
single-threaded engine core fed by lock-free SPSC rings, hot-compiled strategy
DLL plugins, and a Python sidecar for market data (Yahoo Finance, cached in
sqlite).

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
