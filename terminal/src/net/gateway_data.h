#pragma once
// Chart/watchlist/backtest data via the IBKR Client Portal Gateway —
// replaces the old Python/Yahoo sidecar with the same session that routes
// orders. Candles come from /iserver/marketdata/history, watchlist quotes
// from periodic /iserver/marketdata/snapshot polls, and session state
// doubles as the app-wide connectivity indicator and the Sign-In modal's
// live status.
//
// Callback surface mirrors the old IpcClient: callbacks fire on the worker
// thread; consumers are already mutex-guarded stores.

#include "market_data.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tt::net {

struct CandleBatch {
    uint32_t id = 0;
    std::string symbol, interval;
    bool cached = false;
    std::vector<Candle> candles;
};

class GatewayData {
public:
    struct Callbacks {
        std::function<void(CandleBatch&&)> on_candles;
        std::function<void(const std::string& symbol, const Quote&)> on_tick;
        std::function<void(uint32_t id, std::string code, std::string message)> on_error;
        std::function<void(std::string)> on_log;
    };

    explicit GatewayData(std::string gateway_url);   // .../v1/api
    ~GatewayData();

    void start(Callbacks cbs);
    void stop();

    // True while the gateway session is authenticated.
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    // Bumped on every authenticated (re)connect; lets panels re-request.
    uint64_t connection_generation() const {
        return conn_gen_.load(std::memory_order_relaxed);
    }
    // For the Sign-In modal: brokerage account id ("" if no session), and the
    // https root to open in a browser for the gateway's login page.
    std::string account() const;
    std::string login_url() const;
    // Every tradeable (sub-)account under the login, in gateway order. Size <= 1
    // means a single account (no sub-accounts).
    std::vector<std::string> accounts() const;

    // Whether the live session is a paper or a real-money account (from the
    // gateway's isPaper flag); Unknown until a session is established.
    enum class AccountKind { Unknown, Paper, Live };
    AccountKind account_kind() const {
        return account_kind_.load(std::memory_order_acquire);
    }

    // Thread-safe; return the request id used (0 if not running).
    uint32_t request_candles(const std::string& symbol, const std::string& interval,
                             const std::string& range);
    uint32_t subscribe_quotes(const std::vector<std::string>& symbols, int poll_s);
    void unsubscribe(uint32_t sub_id);

private:
    struct CandleReq {
        uint32_t id;
        std::string symbol, interval, range;
    };

    void worker();
    int64_t conid_for(const std::string& symbol);   // worker thread; 0 = unknown
    void log(std::string msg);

    std::string gateway_url_;
    Callbacks cbs_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> conn_gen_{0};
    std::atomic<uint32_t> next_id_{1};

    std::atomic<AccountKind> account_kind_{AccountKind::Unknown};

    mutable std::mutex mu_;   // guards account_, accounts_, reqs_, subs_
    std::string account_;
    std::vector<std::string> accounts_;   // all tradeable (sub-)accounts
    std::vector<CandleReq> reqs_;
    std::unordered_map<uint32_t, std::vector<std::string>> subs_;
    int poll_s_ = 5;

    void* rest_ = nullptr;   // CURL*, worker thread only
    std::unordered_map<std::string, int64_t> conids_;
};

} // namespace tt::net
