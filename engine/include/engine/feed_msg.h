#pragma once
// Vendor-neutral normalized feed messages. Every market-data adapter parses
// its wire format into these; the handshake/stream loops are then identical
// regardless of vendor.

#include <cstdint>
#include <string>

namespace tt {

struct FeedMsg {
    enum Kind : uint8_t { None = 0, Trade, Quote, Connected, Authenticated,
                          Subscription, Error };
    Kind kind = None;
    uint32_t symbol_id = 0;      // 0 = not in this session's table
    double price = 0.0, size = 0.0;      // Trade
    double bid = 0.0, ask = 0.0;         // Quote
    int64_t ts_ns = 0;
    std::string error;                   // Error
};

// One historical bar from a vendor REST endpoint (gap backfill).
struct RestBar {
    int64_t ts_ns = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

} // namespace tt
