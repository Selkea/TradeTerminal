# Writing TradeTerminal strategies

A strategy is one `.cpp` file in this folder. The terminal compiles it with
g++ and hot-loads the DLL — edit → **Build** (Strategy Manager panel) → run a
backtest, without restarting the app.

## Included strategies

| File | Style | Entry / exit | Risk control |
|---|---|---|---|
| `sma_crossover.cpp` | example | fast/slow SMA cross | none (demo) |
| `donchian_trend.cpp` | trend following | prior N-bar high break / M-bar low break | ATR position sizing + trailed resting stop |
| `bollinger_reversion.cpp` | mean reversion | z ≤ −entry / z back inside band | time stop + trend filter (no price stop, on purpose) |
| `orb_breakout.cpp` | intraday breakout | opening-range stop entries / bracket TP-SL | range-based sizing, manual OCO, EOD flatten |
| `rsi2_pullback.cpp` | pullback (Connors) | RSI(2) dip in uptrend / close > exit SMA | regime filter + small fixed allocation |
| `scalper_burst.cpp` | scalping (tick-driven) | burst_bps move within window_s of prints / TP-SL bracket | manual OCO + time-stop + cooldown; validate via tick Replay, never candle backtests |

## Promoted (built-in) strategies

`orb_breakout.cpp`, `donchian_trend.cpp`, `rsi2_pullback.cpp`,
`bollinger_reversion.cpp`, and `scalper_burst.cpp` are also compiled directly
into `tt_terminal` ("promoted") — see `TT_PROMOTED_STRATEGIES` in
`terminal/CMakeLists.txt`. This is a **build-only** change: the `.cpp` files
above are byte-for-byte identical either way and still hot-rebuild via the
Strategy panel's Build button same as any other file here (a rebuilt DLL for
a promoted key is just inert — the terminal always prefers the compiled-in
version). Promotion exists so the live-trading process on the VPS doesn't
depend on the MSYS2/g++ toolchain or this directory being present at runtime.

Promoting a strategy is purely a CMakeLists.txt edit — add its path to
`TT_PROMOTED_STRATEGIES`, nothing in the `.cpp` changes. One rule that only
applies to promoted strategies: **a promoted strategy's class name must be
unique across every other promoted strategy.** Hot-loaded DLLs never collide
(each is its own binary), but promoted strategies share `tt_terminal`'s one
link unit — a duplicate class name there is a silent ODR violation (wrong
strategy logic quietly running under one of the keys, no compiler/linker
diagnostic), not a build error. `terminal/CMakeLists.txt` fails configure on
a detected duplicate as a guard, but keep names distinct regardless.

Patterns worth copying from them: protective stops are **resting Stop orders
the strategy owns** (id known → cancellable → trailable), never bracket legs
(`take_profit`/`stop_loss` spawn engine-side orders whose ids the strategy
never learns, so it can't cancel or trail them — fine for fire-and-forget,
wrong for anything managed); exits/entries are guarded by in-flight order ids
so a signal can't double-fire while a market order awaits its fill; and every
`on_fill` routes by order id, not by side.

## Anatomy

```cpp
#include "tt/strategy_api.h"
using namespace tt;

namespace { constexpr ParamDesc kParams[] = { {"period", 20, 1, 500} }; }

class MyStrategy final : public IStrategy {
    void on_init(IStrategyContext& ctx) noexcept override { /* read params, reset ALL state */ }
    void on_bar (IStrategyContext& ctx, uint32_t sym, const Bar&)  noexcept override { /* trade */ }
    void on_tick(IStrategyContext& ctx, uint32_t sym, const Tick&) noexcept override {}
    void on_fill(IStrategyContext& ctx, const Fill&) noexcept override {}
    void on_stop(IStrategyContext& ctx) noexcept override {}
    void destroy() noexcept override { delete this; }
};

TT_STRATEGY(MyStrategy, "My Strategy", kParams)
```

Orders: `ctx.submit_order({sym, Side::Buy, OrdType::Market, {}, qty, 0.0, 0.0, 0.0, 0.0})`
(returns order id, 0 = rejected; initialize every field — the v2 struct ends
with stop_price/take_profit/stop_loss and `-Wextra` flags partial init).
Limit orders set `OrdType::Limit` and the limit price; Stop orders set
`OrdType::Stop` and the stop trigger. State: `ctx.position(sym)`, `ctx.cash()`, `ctx.now_ns()`.
Params declared in `kParams` appear as editable fields in the UI and are read
with `ctx.param("name", fallback)`.

## Rules (the ABI contract)

1. **Reset everything in `on_init`** — the same instance may run many backtests.
2. **No exceptions may escape a callback** (they're all `noexcept` — catch your own).
3. **No threads, no `atexit`,** no globals with destructors that touch the host.
4. Only POD types cross the boundary; anything you `new`, you free (in `destroy()`).
5. All callbacks arrive on the single engine thread — no locks needed, and
   blocking in a callback stalls the whole engine.
6. Don't link extra libraries or change compiler flags; the terminal builds
   with `-std=c++20 -O2 -g -shared` against `sdk/include` only.

## Determinism

Backtests are bit-identical across reruns: one engine thread, a replayed
clock, and seeded execution randomness. Keep your strategy deterministic too
(no wall-clock reads, no `rand()` without a fixed seed) and reruns stay
reproducible.
