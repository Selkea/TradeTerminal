#pragma once
// Finnhub.io real-time US trade stream -> engine ticks. Same IFeedHandler
// shape as the other feeds: one I/O thread owns the websocket, trades become
// EngineEvent Ticks. Finnhub's free tier streams real-time US trade prints
// with no residency gate (data vendor, not a broker — you just sign up for an
// API key), which suits a Canadian user who routes orders through IBKR.
//
// Wire format differs from Polygon in three ways:
//   - the token rides the URL: wss://ws.finnhub.io?token=KEY (no auth frame);
//   - symbols are subscribed one JSON message each,
//     {"type":"subscribe","symbol":"AAPL"} (no subscribe ack);
//   - trades arrive as {"type":"trade","data":[{s,p,t,v},...]}; errors as
//     {"type":"error","msg":...}; the server also sends {"type":"ping"}.
// The websocket carries trades only (no quotes), so ticks leave bid/ask at 0.
// There is no REST gap-backfill on reconnect (free candles are restricted);
// the stream simply resumes.

#include "engine/feed_msg.h"   // FeedMsg
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

struct FinnhubFeedConfig {
    std::string api_key;
    std::string stream_url = "wss://ws.finnhub.io";
    std::string rest_url = "https://finnhub.io/api/v1";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
    bool busy_poll = false;             // spin instead of select()
    int pin_core = -1;
    int bar_seconds = 60;               // accepted for parity; unused (no backfill)
};

// Parses one websocket frame into normalized messages; returns the number
// appended. Exposed for unit tests.
//   {"type":"trade","data":[{s,p,t,v}...]}  {"type":"error","msg":...}  ping
size_t finnhub_parse_feed_msgs(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               std::vector<FeedMsg>& out);

// Synchronous API-key check (blocks up to ~10 s — worker thread only).
// True = key valid; detail carries a summary or the error.
bool finnhub_verify_key(const std::string& rest_url, const std::string& api_key,
                        std::string& detail);

class FinnhubFeed final : public IFeedHandler {
public:
    using Sink = std::function<bool(const EngineEvent&)>;

    FinnhubFeed(FinnhubFeedConfig cfg, Sink sink);
    ~FinnhubFeed() override;

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

    FinnhubFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;
};

} // namespace tt
