#pragma once
// Chart/watchlist/backtest data straight from IB Gateway over the TWS socket
// API — the same IMarketData surface as GatewayData, but with no Client
// Portal gateway and no web (IBeam) login. When trading routes through TWS
// the whole app rides that one brokerage session: candles come from
// reqHistoricalData, watchlist quotes from streaming reqMktData (served
// delayed automatically when the account has no market-data subscription).
//
// Same I/O shape as TwsFeed/TwsBroker: one thread owns the connection and
// auto-reconnects; UI-thread requests are queued under a mutex and pumped on
// the I/O thread.

#include "net/market_source.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt::net {

class TwsData final : public IMarketData {
public:
    TwsData() = default;
    ~TwsData() override;

    // Call before start(); the port follows the active account's mode
    // (IB Gateway: 4002 paper, 4001 live).
    void set_endpoint(std::string host, int port, int client_id = 9);

    void start(Callbacks cbs);
    void stop();

    bool connected() const override {
        return connected_.load(std::memory_order_acquire);
    }
    uint64_t connection_generation() const override {
        return conn_gen_.load(std::memory_order_relaxed);
    }

    uint32_t request_candles(const std::string& symbol, const std::string& interval,
                             const std::string& range) override;
    uint32_t subscribe_quotes(const std::vector<std::string>& symbols,
                              int poll_s) override;
    void unsubscribe(uint32_t sub_id) override;

    std::string account() const override;
    std::vector<std::string> accounts() const override;
    AccountKind account_kind() const override {
        return account_kind_.load(std::memory_order_acquire);
    }

private:
    struct Io;   // defined in tws_data.cpp; owns all TWS API state

    struct CandleReq {
        uint32_t id;
        std::string symbol, interval, range;
    };

    void io_loop();
    void log(std::string msg);
    void wake();   // nudge the I/O thread so queued requests go out promptly

    Callbacks cbs_;

    // Endpoint is fixed before start() and read-only afterwards.
    std::string host_ = "127.0.0.1";
    int port_ = 4002;
    int client_id_ = 9;   // distinct from TwsBroker (7) and TwsFeed (8)

    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> conn_gen_{0};
    std::atomic<uint32_t> next_id_{1};
    std::atomic<void*> wake_{nullptr};   // EReaderOSSignal* while I/O thread runs

    std::atomic<AccountKind> account_kind_{AccountKind::Unknown};

    mutable std::mutex mu_;   // guards account_, accounts_, reqs_, want_syms_
    std::string account_;
    std::vector<std::string> accounts_;
    std::vector<CandleReq> reqs_;          // pending candle fetches
    std::vector<std::string> want_syms_;   // desired quote-stream set
    bool want_dirty_ = false;
    uint32_t quote_sub_ = 0;               // last subscribe_quotes id
};

} // namespace tt::net
