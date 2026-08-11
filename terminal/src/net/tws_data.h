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

#include "net/gateway_auth.h"
#include "net/market_source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt::net {

// A market-scanner request (IBKR reqScannerSubscription). Defaults ask for the
// most-active US stocks; the daily auto-lineup re-ranks the returned pool by
// realized volatility itself, so the scan just needs to be a liquid, tradeable
// candidate set rather than the final ordering.
struct ScanSpec {
    std::string scan_code = "MOST_ACTIVE";    // IBKR scanCode
    std::string location = "STK.US.MAJOR";    // locationCode
    std::string instrument = "STK";
    int rows = 30;                            // numberOfRows
    double price_above = 0;                   // abovePrice filter (0 = unset)
    double volume_above = 0;                  // aboveVolume filter (0 = unset)
};

// One scanner result row: the qualified contract's symbol + primary exchange
// and its rank in the scan (0 = top).
struct ScanHit {
    std::string symbol;
    std::string exchange;
    int rank = 0;
};

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
    int pending_history() const override {
        return pending_hist_.load(std::memory_order_relaxed);
    }
    int oldest_history_age_ms() const override {
        return oldest_hist_ms_.load(std::memory_order_relaxed);
    }
    // Another program owns this client id on the gateway (IB error 326). Latched
    // for the life of the process: see engine/tws_client_id.h and io_loop.
    bool client_id_conflict() const override {
        return client_id_conflict_.load(std::memory_order_acquire);
    }
    HistStats hist_stats() const override {
        HistStats s;
        s.cache_served = hs_cache_served_.load(std::memory_order_relaxed);
        s.cache_fetched = hs_cache_fetched_.load(std::memory_order_relaxed);
        s.cache_lookups = hs_cache_lookups_.load(std::memory_order_relaxed);
        s.requests_sent = hs_requests_sent_.load(std::memory_order_relaxed);
        s.held_min_gap = hs_held_min_gap_.load(std::memory_order_relaxed);
        s.held_identical = hs_held_identical_.load(std::memory_order_relaxed);
        s.held_budget = hs_held_budget_.load(std::memory_order_relaxed);
        s.abandoned = hs_abandoned_.load(std::memory_order_relaxed);
        return s;
    }

    // Gateway login state, from the data farms (see net/gateway_auth.h). NOT
    // inline: the age is measured off a steady clock owned by the .cpp, and the
    // policy object takes "now" as an argument so it stays testable.
    bool gateway_authed() const override;
    int64_t gateway_auth_age_ms() const override;
    int gateway_farms_ok() const override;

    uint32_t request_candles(const std::string& symbol, const std::string& interval,
                             const std::string& range,
                             ReqPriority prio = ReqPriority::Bulk) override;
    uint32_t subscribe_quotes(const std::vector<std::string>& symbols,
                              int poll_s) override;
    void unsubscribe(uint32_t sub_id) override;

    // One-shot market scan. `cb` fires once on the I/O thread with the ranked
    // hits (empty if the scan failed or the session dropped) and must be
    // thread-safe. Only one scan runs at a time; a new request supersedes any
    // in flight. Returns the request id, or 0 if the source isn't running.
    using ScanCb = std::function<void(std::vector<ScanHit>)>;
    uint32_t request_scan(const ScanSpec& spec, ScanCb cb);

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
        ReqPriority prio = ReqPriority::Bulk;
        // When the UI thread queued it (steady ms). A request the pacing gate
        // holds is put BACK on this queue every pass, so without a stamp it can
        // wait indefinitely and then spend a send slot on an answer nobody is
        // waiting for any more. See kHistQueueMaxWaitMs.
        int64_t queued_ms = 0;
        // Which SendHold reasons this request has already been COUNTED under
        // (bit 1<<int(SendHold)). A held request is re-offered to the gate on
        // every io_loop pass, so without this the hold counters in HistStats
        // would measure how long a hold lasted rather than how many requests it
        // held. The field rides the request back onto reqs_ with the rest of it.
        uint8_t held_mask = 0;
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
    // Another program holds client_id_ on this gateway (IB error 326). Written
    // once by the I/O thread, read by the UI thread (/diag) and by io_loop,
    // which parks instead of reconnecting. Never cleared: the condition can only
    // end when a HUMAN closes the other program, and an app that silently came
    // back to life at an unobserved moment would be the third variant of the
    // lie this project has already shipped twice (oldest_history_age_ms pinned
    // at 0 through a 5-hour outage; data.connected true against a login modal).
    std::atomic<bool> client_id_conflict_{false};
    std::atomic<uint64_t> conn_gen_{0};
    std::atomic<uint32_t> next_id_{1};
    std::atomic<void*> wake_{nullptr};   // EReaderOSSignal* while I/O thread runs

    // Published by the I/O thread each loop for /diag: in-flight history
    // fetches and the oldest one's age (ms). A half-open data session (socket
    // up, reqHistoricalData answers silently dropped — e.g. after the nightly
    // gateway restart) shows oldest_hist_ms_ climbing without bound.
    std::atomic<int> pending_hist_{0};
    std::atomic<int> oldest_hist_ms_{0};

    // Cumulative fetch accounting, published by the I/O thread as it happens and
    // read from the UI thread (see HistStats in net/market_source.h). Written by
    // exactly one thread, so relaxed ordering is enough: nothing else is
    // published through them and a reader that sees a counter one increment
    // stale has read a number that was true a moment ago.
    std::atomic<uint64_t> hs_cache_served_{0};
    std::atomic<uint64_t> hs_cache_fetched_{0};
    std::atomic<uint64_t> hs_cache_lookups_{0};
    std::atomic<uint64_t> hs_requests_sent_{0};
    std::atomic<uint64_t> hs_held_min_gap_{0};
    std::atomic<uint64_t> hs_held_identical_{0};
    std::atomic<uint64_t> hs_held_budget_{0};
    std::atomic<uint64_t> hs_abandoned_{0};

    std::atomic<AccountKind> account_kind_{AccountKind::Unknown};

    // Is the GATEWAY logged in to IBKR? connected_ above cannot answer that: a
    // gateway sitting on the "UNRECOGNIZED USERNAME OR PASSWORD" modal still
    // listens on the API port. Fed from the farm-status codes and successful
    // history deliveries on the I/O thread; read from the UI thread (/diag,
    // /metrics, the pre-open check) — the class carries its own mutex.
    GatewayAuth auth_;

    mutable std::mutex mu_;   // guards account_, accounts_, reqs_, want_syms_
    std::string account_;
    std::vector<std::string> accounts_;
    std::vector<CandleReq> reqs_;          // pending candle fetches
    std::vector<std::string> want_syms_;   // desired quote-stream set
    bool want_dirty_ = false;
    uint32_t quote_sub_ = 0;               // last subscribe_quotes id

    // Pending scan request + its callback (UI thread sets, I/O thread drains).
    // Only the newest is kept; a second request before the first is pumped
    // replaces it.
    struct ScanReq {
        uint32_t id = 0;
        ScanSpec spec;
        ScanCb cb;
    };
    bool scan_pending_ = false;
    ScanReq scan_req_;
};

} // namespace tt::net
