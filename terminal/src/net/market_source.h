#pragma once
// The UI's market-data backbone: charts, watchlist quotes, and
// backtest/sweep candles all flow through this interface. Two sources
// implement it — GatewayData (IBKR Client Portal web gateway) and TwsData
// (IB Gateway over the TWS socket) — and App picks one at startup from the
// persisted trade route. Only one runs at a time: IBKR allows a single
// brokerage session per username, so the CP web session and a TWS session
// under the same login endlessly kick each other if both are held.

#include "market_data.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tt::net {

struct CandleBatch {
    uint32_t id = 0;
    std::string symbol, interval;
    bool cached = false;
    std::vector<Candle> candles;
};

// Paper/live flag of the connected brokerage session; Unknown = no session.
enum class AccountKind { Unknown, Paper, Live };

class IMarketData {
public:
    // Callbacks fire on the source's worker thread; consumers are already
    // mutex-guarded stores.
    struct Callbacks {
        std::function<void(CandleBatch&&)> on_candles;
        std::function<void(const std::string& symbol, const Quote&)> on_tick;
        std::function<void(uint32_t id, std::string code, std::string message)>
            on_error;
        std::function<void(std::string)> on_log;
    };

    virtual ~IMarketData() = default;

    // True while a session is up and data requests can be served.
    virtual bool connected() const = 0;
    // Bumped on every (re)connect; lets panels re-request what they show.
    virtual uint64_t connection_generation() const = 0;

    // Diagnostics: how many candle (historical) fetches are outstanding, and
    // the age of the oldest in milliseconds (0 when none). A source whose
    // socket stays up but silently stops answering history requests shows a
    // steadily climbing oldest age here. Default 0 for sources that don't
    // track it (only TwsData does today).
    virtual int pending_history() const { return 0; }
    virtual int oldest_history_age_ms() const { return 0; }

    // Thread-safe; return the request id used (0 if not running).
    virtual uint32_t request_candles(const std::string& symbol,
                                     const std::string& interval,
                                     const std::string& range) = 0;
    // Subscribe the given set; the newest subscription defines what streams.
    // poll_s is advisory (polling sources only).
    virtual uint32_t subscribe_quotes(const std::vector<std::string>& symbols,
                                      int poll_s) = 0;
    virtual void unsubscribe(uint32_t sub_id) = 0;

    // Session identity for the Account menu / PAPER-LIVE badges.
    virtual std::string account() const = 0;
    virtual std::vector<std::string> accounts() const = 0;
    virtual AccountKind account_kind() const = 0;
};

} // namespace tt::net
