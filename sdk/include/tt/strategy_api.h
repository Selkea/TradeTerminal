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

// v2: OrderRequest grew stop_price + bracket legs (take_profit/stop_loss),
// OrdType gained Stop. Old DLLs are rejected by the version check.
#define TT_SDK_VERSION 2u

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
#define TT_STRATEGY(CLS, NAME, PARAMS)                                                   \
    extern "C" __declspec(dllexport) uint32_t tt_sdk_version() { return TT_SDK_VERSION; } \
    extern "C" __declspec(dllexport) const tt::StrategyInfo* tt_strategy_info() {         \
        static const tt::StrategyInfo info{TT_SDK_VERSION, NAME, PARAMS,                  \
                                           sizeof(PARAMS) / sizeof(PARAMS[0])};           \
        return &info;                                                                     \
    }                                                                                     \
    extern "C" __declspec(dllexport) tt::IStrategy* tt_create_strategy() { return new CLS(); }
