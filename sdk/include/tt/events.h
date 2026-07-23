#pragma once
// TradeTerminal SDK — plain-old-data event types shared between the engine and
// strategy DLLs. Everything crossing the DLL boundary must stay POD: no
// std::string, no containers, no virtual members. Layouts are static_asserted
// so an accidental field change breaks the build, not the ABI.

#include <cstdint>

namespace tt {

enum class Side : uint8_t { Buy = 1, Sell = 2 };
enum class OrdType : uint8_t { Market = 1, Limit = 2, Stop = 3 };
enum class OrdStatus : uint8_t { New, Accepted, Filled, PartFilled, Cancelled, Rejected };

struct Bar {
    int64_t ts_ns;      // bar open time, epoch nanoseconds
    double  open, high, low, close, volume;
};
static_assert(sizeof(Bar) == 48);

struct Tick {
    int64_t ts_ns;      // epoch nanoseconds
    double  price, size, bid, ask;
};
static_assert(sizeof(Tick) == 40);

struct OrderRequest {
    uint32_t symbol_id;
    Side     side;
    OrdType  type;
    // 1 = allow fills outside regular trading hours (extended-hours). IBKR
    // requires a Limit order for this; a Market order outside RTH is rejected.
    // (One byte reclaimed from padding — the struct is still 48 bytes.)
    uint8_t  outside_rth{};
    double   qty;
    double   limit_price;   // Limit only
    double   stop_price;    // Stop only: trigger price
    // Bracket exit legs, spawned when this order fills (0 = none). The two
    // children are one-cancels-other.
    double   take_profit;   // exit limit price
    double   stop_loss;     // exit stop trigger
};
static_assert(sizeof(OrderRequest) == 48);

struct Fill {
    uint64_t order_id;
    uint32_t symbol_id;
    Side     side;
    uint8_t  _pad[3]{};
    int64_t  ts_ns;
    double   price, qty, fee;
};
static_assert(sizeof(Fill) == 48);

struct Position {
    uint32_t symbol_id;
    double   qty;            // signed; negative = short
    double   avg_price;
    double   unrealized_pnl;
    double   realized_pnl;
};

} // namespace tt
