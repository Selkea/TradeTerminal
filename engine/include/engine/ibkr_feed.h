#pragma once
// IBKR market data -> engine ticks, via the same Client Portal Gateway the
// IbkrBroker uses. Zero additional data bill during the validation phase:
// the gateway streams top-of-book "smd" updates over its websocket.
//
// Honest quality note: IBKR conflates this feed to ~250 ms top-of-book
// snapshots — perfect for bar-based strategies and pipeline validation,
// wrong for tick-scale work. Message rates are tiny (a few per second per
// symbol), so parsing uses nlohmann rather than the simdjson hot path the
// SIP-rate feeds get.
//
// Same IFeedHandler contract as the other feeds: I/O thread owns the
// socket, trades become Tick events with cached bid/ask attached, drops are
// counted, reconnects backfill via /iserver/marketdata/history.

#include "engine/feed_msg.h"   // RestBar (shared backfill shape)
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

struct IbkrFeedConfig {
    std::string gateway_url = "https://localhost:5000/v1/api";
    std::string ws_url = "wss://localhost:5000/v1/api/ws";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
    int bar_seconds = 60;               // gap backfill granularity
    int pin_core = -1;
};

// ---- parsers (exposed for unit tests) --------------------------------------

// POST /tickle response -> gateway session token ("" if absent). The token
// rides the websocket upgrade as cookie "api=<token>".
std::string ibkr_parse_session_token(std::string_view json_text);

// One websocket message. Market updates carry numeric field ids:
// 31 = last price, 84 = bid, 86 = ask, 7059 = last size; only changed
// fields are present, so the feed keeps a per-symbol cache.
struct IbkrMdUpdate {
    enum Kind : uint8_t { None = 0, Market, AuthLost };
    Kind kind = None;
    int64_t conid = 0;
    bool has_last = false, has_bid = false, has_ask = false, has_size = false;
    double last = 0, bid = 0, ask = 0, size = 0;
    int64_t ts_ms = 0;   // "_updated", Unix ms (0 if absent)
};
bool ibkr_parse_md_msg(std::string_view json_text, IbkrMdUpdate& out);

// GET /iserver/marketdata/history response ("data":[{t,o,h,l,c,v}...]).
bool ibkr_parse_history_bars(std::string_view json_text, std::vector<RestBar>& out);

// ----------------------------------------------------------------------------

class IbkrFeed final : public IFeedHandler {
public:
    using Sink = std::function<bool(const EngineEvent&)>;

    IbkrFeed(IbkrFeedConfig cfg, Sink sink);
    ~IbkrFeed() override;

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

    IbkrFeedConfig cfg_;
    Sink sink_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;
};

} // namespace tt
