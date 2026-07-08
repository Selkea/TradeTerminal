#pragma once
// IBKR real-time market data via the TWS socket API -> engine ticks. Same
// IFeedHandler shape as the other feeds: one I/O thread owns the connection,
// trades become EngineEvent Ticks with the latest bid/ask attached.
//
// Data strategy per symbol: tick-by-tick "Last" + "BidAsk" (true per-print
// data, requires a real-time market-data subscription; IBKR allows only a few
// simultaneous tick-by-tick streams by default). If a tick-by-tick request is
// refused (no subscription / stream limit), the symbol falls back to streaming
// reqMktData — IBKR-conflated ~250 ms updates, still push-based, and served
// delayed automatically when the account lacks the subscription.

#include "engine/events.h"
#include "engine/feed.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt {

struct TwsFeedConfig {
    std::string host = "127.0.0.1";
    int port = 4002;        // IB Gateway: 4002 paper, 4001 live
    int client_id = 8;      // distinct from the broker's client id
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
};

class TwsFeed final : public IFeedHandler {
public:
    using Sink = std::function<bool(const EngineEvent&)>;

    TwsFeed(TwsFeedConfig cfg, Sink sink);
    ~TwsFeed() override;

    bool start() override;
    void stop() override;
    // v1: the symbol set is fixed at construction (session-scoped feed).
    void subscribe(const std::vector<std::string>&) override {}
    void unsubscribe(const std::vector<std::string>&) override {}

    bool connected() const override { return connected_.load(std::memory_order_acquire); }
    uint64_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

    bool pop_log(std::string& out);

private:
    struct Io;   // defined in tws_feed.cpp; owns all TWS API state

    void io_loop();
    void log(std::string line);

    TwsFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<void*> wake_{nullptr};   // EReaderOSSignal* while the I/O thread runs

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;
};

} // namespace tt
