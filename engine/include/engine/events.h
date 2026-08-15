#pragma once
// Internal engine event: exactly one cache line. Everything on the hot path
// moves through SpscRing<EngineEvent, N> with zero allocation.

#include <cstdint>

namespace tt {

enum class EvType : uint16_t {
    Tick = 1, Bar, OrderNew, OrderCancel, Fill, FeedStatus, End,
    PosSnap, AcctSnap,   // broker reconciliation at session start
    ReconcileEnd         // broker finished replaying positions/orders/cash
};

// EngineEvent.flags bits.
constexpr uint16_t kEvFlagRejected = 1;   // OrderCancel: rejected, not cancelled
constexpr uint16_t kEvFlagProtective = 2; // + a protective stop leg: symbol_id is set,
                                          // the position it guarded is now naked

struct alignas(64) EngineEvent {
    uint16_t type;            // EvType
    uint16_t flags;
    uint32_t symbol_id;
    int64_t ts_event_ns;      // exchange/replay time — drives BacktestClock
    int64_t ts_ingest_tsc;    // raw rdtsc at ring entry — latency measurement

    union {
        struct { double price, size, bid, ask; uint64_t _r; } tick;
        struct { double open, high, low, close, volume; } bar;
        struct { uint64_t order_id; double qty, limit_price;
                 uint8_t side, ord_type; uint8_t _p[6]; } order;
        struct { uint64_t order_id; double price, qty, fee; uint8_t side;
                 uint8_t _p[7]; } fill;
        struct { double qty, avg_price; } pos;    // PosSnap
        struct { double cash; } acct;             // AcctSnap
        // ReconcileEnd: non-zero STOCK positions the broker reported for symbols
        // this session does not trade. They are NOT adopted — PosSnap is keyed
        // on a session symbol id and has no bucket for an unrecognised symbol —
        // so this count is the only thing that crosses into the engine, and it
        // exists so "broker reconciliation complete — 0 symbol(s) held" can stop
        // being said while the account holds 20 shares of something.
        // See engine/reconcile_policy.h.
        struct { int32_t offlineup; } recon;   // -1 = the position stream never answered
    } u{};
};
static_assert(sizeof(EngineEvent) == 64, "one cache line per event");

} // namespace tt
