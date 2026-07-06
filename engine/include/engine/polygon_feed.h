#pragma once
// Polygon.io real-time market data -> engine ticks. Same IFeedHandler shape
// as the other feeds: one I/O thread owns the websocket, trades become
// EngineEvent Ticks (quotes cached and attached as bid/ask), reconnects
// backfill the gap with REST aggregate bars.
//
// Chosen as the data vendor for the post-Alpaca world (no residency
// restrictions): wss://socket.polygon.io/stocks speaks JSON arrays over one
// socket, so the parsing rides the same simdjson hot path. Databento is the
// later alternative if its binary DBN protocol's headroom is ever needed.
//
// Message kinds are normalized into FeedMsg — the two vendors' events
// map 1:1 (trade/quote/connected/authenticated/subscription/error), and
// sharing the struct keeps every feed's handshake/stream loop identical.

#include "engine/feed_msg.h"   // FeedMsg, RestBar
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

struct PolygonFeedConfig {
    std::string api_key;
    std::string stream_url = "wss://socket.polygon.io/stocks";
    std::string rest_url = "https://api.polygon.io";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
    bool busy_poll = false;             // spin instead of select() (SIP-rate)
    int pin_core = -1;
    int bar_seconds = 60;               // gap backfill granularity
};

// Parses one websocket frame (a JSON array of events) into normalized
// messages; returns the number appended. Exposed for unit tests.
//   {"ev":"T",...} trade   {"ev":"Q",...} quote   {"ev":"status",...} control
size_t polygon_parse_feed_msgs(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               std::vector<FeedMsg>& out);

// GET /v2/aggs/... response ("results":[{t,o,h,l,c,v}...]) -> bars.
bool polygon_parse_rest_bars(std::string_view json_text, std::vector<RestBar>& out);

// Synchronous API-key check (blocks up to ~10 s — worker thread only).
// True = key valid; detail carries a summary or the error.
bool polygon_verify_key(const std::string& rest_url, const std::string& api_key,
                        std::string& detail);

class PolygonFeed final : public IFeedHandler {
public:
    using Sink = std::function<bool(const EngineEvent&)>;

    PolygonFeed(PolygonFeedConfig cfg, Sink sink);
    ~PolygonFeed() override;

    bool start() override;
    void stop() override;
    // v1: the symbol set is fixed at construction (session-scoped feed).
    void subscribe(const std::vector<std::string>&) override {}
    void unsubscribe(const std::vector<std::string>&) override {}

    bool connected() const override { return connected_.load(std::memory_order_acquire); }
    uint64_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

    bool pop_log(std::string& out);

private:
    void io_loop();
    void log(std::string line);

    PolygonFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;
};

} // namespace tt
