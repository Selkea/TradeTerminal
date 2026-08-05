#pragma once
// TradeTerminal strategy SDK. Strategies implement IStrategy against this
// header and (from Phase 3 on) are compiled to DLLs loaded by the terminal.
//
// ABI rules (enforced by the strategy host, documented in strategies/README):
//  - built with the same MSYS2 UCRT64 g++ as the terminal, dynamic runtime
//  - only POD types and these interfaces cross the boundary
//  - no exception may escape a callback (everything is noexcept)
//  - the DLL frees its own objects (destroy()), the host never deletes them
//  - no threads, no atexit, no globals with destructors touching the host
//  - all callbacks arrive on the single engine thread: no locks needed

#include "events.h"

#include <ctime>

// v4: IStrategy gained on_order_end() — an order dying without filling is now
//     reported (see below).
// v3: IStrategyContext gained budget() — the sizing base every strategy must
//     use instead of cash() (see below).
// v2: OrderRequest grew stop_price + bracket legs (take_profit/stop_loss),
// OrdType gained Stop. Old DLLs are rejected by the version check.
#define TT_SDK_VERSION 4u

namespace tt {

// Local-time hour of day (9.5 = 09:30) for an engine timestamp — the building
// block for time-of-day gates ("only enter between 9.5 and 11"). Uses the
// machine's timezone, so US-market windows assume an Eastern-time box; replays
// of recorded sessions convert with the same rules they traded under.
inline double hour_of_day_local(int64_t ts_ns) noexcept {
    const time_t secs = static_cast<time_t>(ts_ns / 1'000'000'000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    return tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0;
}

// Implemented by the engine; only valid for the duration of a callback.
class IStrategyContext {
public:
    // Returns the order id, or 0 if rejected pre-trade.
    virtual uint64_t submit_order(const OrderRequest&) noexcept = 0;
    virtual bool     cancel_order(uint64_t order_id) noexcept = 0;
    virtual Position position(uint32_t symbol_id) const noexcept = 0;
    virtual double   cash() const noexcept = 0;
    // Dollars this strategy may put into ONE position in `symbol_id`.
    //
    // SIZE AGAINST THIS, NOT cash(). The risk manager caps every entry at a
    // per-position notional (a slice of the daily loss budget) and silently
    // shrinks anything larger, so a percentage of the raw account balance is
    // not a size — it's a number that gets thrown away. On a $1M paper account
    // against a $5k cap, "20% of cash" and "100% of cash" both come back as the
    // same $5k order, which is what made every alloc_pct/risk_pct knob inert.
    // budget() is the number the cap actually allows, so `budget * pct` is a
    // size the engine will honour.
    //
    // Already net of a small cash reserve, so a 100% allocation still leaves
    // room for fees and slippage. Falls back to cash when no cap is configured
    // (a plain backtest). Reducing/closing orders are never capped — an exit
    // can always leave, whatever budget() says.
    virtual double   budget(uint32_t symbol_id) const noexcept = 0;
    // Engine time (backtest or real): epoch nanoseconds.
    virtual int64_t  now_ns() const noexcept = 0;
    // Interns a symbol string to the id used in events.
    virtual uint32_t symbol_id(const char* symbol) noexcept = 0;
    // Strategy parameter by name (UI-editable), with fallback default.
    virtual double   param(const char* name, double fallback) const noexcept = 0;
    // level: 0=debug 1=info 2=warn 3=error
    virtual void     log(int level, const char* msg) noexcept = 0;

protected:
    ~IStrategyContext() = default;
};

class IStrategy {
public:
    virtual void on_init(IStrategyContext& ctx) noexcept = 0;
    virtual void on_bar(IStrategyContext& ctx, uint32_t symbol_id, const Bar& bar) noexcept = 0;
    virtual void on_tick(IStrategyContext& ctx, uint32_t symbol_id, const Tick& tick) noexcept = 0;
    virtual void on_fill(IStrategyContext& ctx, const Fill& fill) noexcept = 0;
    // An order died without completing — rejected by the broker, cancelled from
    // the UI, or dropped because its OCO sibling filled.
    //
    // ANY strategy that stores an order id MUST clear it here. Strategies gate
    // new entries on "an order is in flight" (`if (entry_id_ != 0) return;`),
    // and a dead id is never seen again in on_fill — so an unhandled death
    // wedges that symbol silently for the rest of the session, until the next
    // on_init. That failure mode looks exactly like "the strategy stopped
    // signalling", with nothing in the log to explain it.
    //
    // Not pure: a strategy that only sends market orders it never tracks has
    // nothing to do here. Default is a no-op, i.e. the pre-v4 behaviour.
    virtual void on_order_end(IStrategyContext& ctx, const OrderEnd& e) noexcept {
        (void)ctx;
        (void)e;
    }
    virtual void on_stop(IStrategyContext& ctx) noexcept = 0;
    // DLL-side `delete this` — the host never deletes strategy pointers.
    virtual void destroy() noexcept = 0;

protected:
    virtual ~IStrategy() = default;
};

struct ParamDesc {
    const char* name;
    double def, min, max;
};

struct StrategyInfo {
    uint32_t sdk_version;
    const char* name;
    const ParamDesc* params;
    uint32_t param_count;
};

} // namespace tt

// Every strategy .cpp ends with: TT_STRATEGY(MyClass, "Display Name", params_array)
//
// Two backends, selected by the BUILD, never by the .cpp itself:
//  - default: extern "C" dllexport factory functions, hot-loaded at runtime
//    by StrategyHost from a compiled DLL (strategies/*.cpp via the Strategy
//    panel's Build button).
//  - TT_STRATEGY_STATIC_LINK: self-registers into tt::static_strategy_registry()
//    at static-init time instead, for a strategy compiled directly into
//    tt_terminal ("promoted"). Set ONLY via a per-source CMake compile
//    definition alongside TT_STRATEGY_STATIC_KEY (see terminal/CMakeLists.txt)
//    — the DLL path derives its key from the loaded file's path at runtime;
//    the static path has no runtime path to inspect, so CMake supplies the
//    identical string up front.
#ifdef TT_STRATEGY_STATIC_LINK

#include "tt/strategy_registry.h"

#ifndef TT_STRATEGY_STATIC_KEY
#error "TT_STRATEGY_STATIC_LINK requires TT_STRATEGY_STATIC_KEY (set alongside it via CMake)"
#endif

// Wrapped in an anonymous namespace for true internal linkage: two promoted
// strategies can never collide at link time even if they reused these names
// (unlike the DLL path's extern "C" factory names, which are global).
#define TT_STRATEGY(CLS, NAME, PARAMS)                                                  \
    namespace {                                                                         \
    const tt::StrategyInfo& tt_static_info_##CLS() {                                    \
        static const tt::StrategyInfo info{TT_SDK_VERSION, NAME, PARAMS,                \
                                           sizeof(PARAMS) / sizeof(PARAMS[0])};          \
        return info;                                                                    \
    }                                                                                    \
    tt::IStrategy* tt_static_create_##CLS() { return new CLS(); }                       \
    [[maybe_unused]] const bool tt_static_registered_##CLS =                            \
        tt::detail::register_static_strategy(TT_STRATEGY_STATIC_KEY,                    \
                                             &tt_static_info_##CLS(),                    \
                                             &tt_static_create_##CLS);                   \
    } // namespace

#else

#define TT_STRATEGY(CLS, NAME, PARAMS)                                                   \
    extern "C" __declspec(dllexport) uint32_t tt_sdk_version() { return TT_SDK_VERSION; } \
    extern "C" __declspec(dllexport) const tt::StrategyInfo* tt_strategy_info() {         \
        static const tt::StrategyInfo info{TT_SDK_VERSION, NAME, PARAMS,                  \
                                           sizeof(PARAMS) / sizeof(PARAMS[0])};           \
        return &info;                                                                     \
    }                                                                                     \
    extern "C" __declspec(dllexport) tt::IStrategy* tt_create_strategy() { return new CLS(); }

#endif
