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

    // Force a one-shot drop + reconnect (scheduled daily refresh); the existing
    // reconnect loop re-establishes the stream.
    void request_reconnect();

    bool pop_log(std::string& out);

    // Times the connect-timeout watchdog force-aborted a stuck handshake
    // (see watchdog_loop). Surfaced in /diag alongside the broker's.
    int connect_aborts() const { return connect_aborts_.load(std::memory_order_relaxed); }

    // Another program already holds this feed's TWS API client id (IB error
    // 326). LATCHED and terminal: the I/O loop has stopped reconnecting, so
    // connected() stays false until the app is restarted.
    // See engine/tws_client_id.h.
    bool client_id_conflict() const {
        return client_id_conflict_.load(std::memory_order_acquire);
    }

private:
    struct Io;   // defined in tws_feed.cpp; owns all TWS API state

    void io_loop();
    void watchdog_loop();
    // Force-abort an in-flight eConnect that hasn't handshaked yet by closing
    // its socket from off the I/O thread (unblocks the blocking eConnect read).
    // See TwsBroker::abort_inflight_connect — same rationale for the feed.
    void abort_inflight_connect(const char* why, int64_t only_if_started_ms = 0);
    void log(std::string line);

    TwsFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    // IB error 326, written once by the I/O thread. Never cleared — only a human
    // closing the other program can end it. See engine/tws_client_id.h.
    std::atomic<bool> client_id_conflict_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnect_req_{false};   // scheduled daily refresh
    std::atomic<uint64_t> dropped_{0};
    std::atomic<void*> wake_{nullptr};   // EReaderOSSignal* while the I/O thread runs

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    // Connect-timeout watchdog (see TwsBroker for the full rationale): the feed's
    // eConnect can freeze the same way; a wedged data session during IBKR's
    // overnight maintenance was part of the same outage.
    std::mutex conn_mu_;
    void* connecting_ = nullptr;                  // EClientSocket* in flight (guarded by conn_mu_)
    std::atomic<int64_t> connect_started_ms_{0};  // steady-clock ms when eConnect began; 0 = idle
    std::atomic<int> connect_aborts_{0};

    std::thread io_thread_;
    std::thread watchdog_thread_;
};

} // namespace tt
