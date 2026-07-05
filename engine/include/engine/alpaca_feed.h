#pragma once
// Alpaca real-time market data (IEX on the free tier) -> engine ticks.
// Implements the IFeedHandler seam: one I/O thread owns the websocket,
// normalizes trade messages into EngineEvent Ticks, and hands them to a
// sink (Engine::push_feed_event) — never blocking, dropping + counting when
// the ring is full. Quotes are cached per symbol and attached to the next
// trade tick's bid/ask rather than emitted as ticks of their own.
//
// The free feed allows ONE concurrent connection per account; a second
// instance (or another tool) kicks this one off, so reconnect-with-backoff
// is part of the contract here, not an edge case.

#include "engine/events.h"
#include "engine/feed.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tt {

struct AlpacaFeedConfig {
    std::string key_id;
    std::string secret;
    std::string stream_url = "wss://stream.data.alpaca.markets/v2/iex";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
};

// One parsed market-data message (exposed for unit tests). The data stream
// sends JSON *arrays* of these.
struct AlpacaFeedMsg {
    enum Kind : uint8_t { None = 0, Trade, Quote, Connected, Authenticated,
                          Subscription, Error };
    Kind kind = None;
    uint32_t symbol_id = 0;      // 0 = not in this session's table
    double price = 0.0, size = 0.0;      // Trade
    double bid = 0.0, ask = 0.0;         // Quote
    int64_t ts_ns = 0;
    std::string error;                   // Error
};

// Appends every recognized element of the JSON array to out; returns the
// number appended (0 for junk / unrecognized messages).
size_t alpaca_parse_feed_msgs(std::string_view json_text,
                              const std::vector<std::string>& symbols,
                              std::vector<AlpacaFeedMsg>& out);

class AlpacaFeed final : public IFeedHandler {
public:
    // sink: called on the feed's I/O thread with each normalized tick; must
    // not block; returns false when the event was dropped (ring full).
    using Sink = std::function<bool(const EngineEvent&)>;

    AlpacaFeed(AlpacaFeedConfig cfg, Sink sink);
    ~AlpacaFeed() override;

    bool start() override;
    void stop() override;
    // v1: the symbol set is fixed at construction (session-scoped feed).
    void subscribe(const std::vector<std::string>&) override {}
    void unsubscribe(const std::vector<std::string>&) override {}

    bool connected() const override { return connected_.load(std::memory_order_acquire); }
    uint64_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

    // Status/log lines (I/O thread produces, UI drains each frame).
    bool pop_log(std::string& out);

private:
    void io_loop();
    void log(std::string line);

    AlpacaFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;
};

} // namespace tt
