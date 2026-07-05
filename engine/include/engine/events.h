#pragma once
// Internal engine event: exactly one cache line. Everything on the hot path
// moves through SpscRing<EngineEvent, N> with zero allocation.

#include <cstdint>

namespace tt {

enum class EvType : uint16_t {
    Tick = 1, Bar, OrderNew, OrderCancel, Fill, FeedStatus, End,
    PosSnap, AcctSnap   // broker reconciliation at session start
};

// EngineEvent.flags bits.
constexpr uint16_t kEvFlagRejected = 1;   // OrderCancel: rejected, not cancelled

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
    } u{};
};
static_assert(sizeof(EngineEvent) == 64, "one cache line per event");

} // namespace tt
