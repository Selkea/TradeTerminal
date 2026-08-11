#pragma once
// The UI's market-data backbone: charts, watchlist quotes, and
// backtest/sweep candles all flow through this interface. Two sources
// implement it — GatewayData (IBKR Client Portal web gateway) and TwsData
// (IB Gateway over the TWS socket) — and App picks one at startup from the
// persisted trade route. Only one runs at a time: IBKR allows a single
// brokerage session per username, so the CP web session and a TWS session
// under the same login endlessly kick each other if both are held.

#include "market_data.h"
#include "net/hist_pacing.h"   // ReqPriority

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

// Cumulative history-fetch accounting for the life of the process.
//
// Everything here already existed inside the TWS source's I/O thread — the bar
// cache's hit/miss counters (net/bar_cache.h) and the send gate's decisions
// (net/hist_pacing.h) — but only as log prose, so measuring a lineup build meant
// re-deriving it from delivery timestamps afterwards. That is exactly how the
// 2026-08-10 profile had to be reconstructed. Published as numbers so a headless
// dry run (TT_AUTORUN_LINEUP=1) can state them instead of implying them.
//
// EVERYTHING HERE COUNTS REQUESTS, NOT PASSES — the one rule that makes these
// numbers comparable to each other. A request the pacing gate holds is put back
// on the queue and re-offered on every io_loop pass, so any counter incremented
// where the decision is *evaluated* measures how long a hold lasted rather than
// how much work it held.
//
// cache_misses got that wrong and shipped. The 2026-08-11 dry run reported
// cache_hits=24 cache_misses=967 hist_requests=36: 967 "misses" against 36
// actual fetches, because the miss was counted at the cache LOOKUP, which a
// pacing-held request repeats every pass. cache_fetched below is counted at the
// send instead. cache_lookups keeps the old number, correctly named: it measures
// gate churn, not caching.
//
// THE HIT RATE IS cache_served / (cache_served + cache_fetched + abandoned),
// and the third term is not optional. A request the pacing gate gives up on
// (kHistQueueMaxWaitMs) is RESOLVED — errored back to its caller — having only
// ever missed, so it lands in neither of the first two. Dividing by the first
// two alone overstates the cache's share of demand exactly in the degraded,
// saturated builds these counters exist to diagnose: served=24 fetched=36
// abandoned=20 is 30% of the 80 requests made, not the 40% the shorter formula
// reports. (A request with an interval string that will not parse is errored
// before any lookup and is in none of the three; it is a caller bug, not
// history-fetch work, and there has never been a non-zero one in production.)
struct HistStats {
    uint64_t cache_served = 0;    // requests answered from cache — fetches AVOIDED
    uint64_t cache_fetched = 0;   // requests that had to go to the wire (first issues)
    uint64_t cache_lookups = 0;   // raw cache consultations, incl. one per pacing-held pass
    uint64_t requests_sent = 0;   // reqHistoricalData actually put on the wire (incl. retries)
    uint64_t held_min_gap = 0;    // SendHold::MinGap    — 500 ms spacing
    uint64_t held_identical = 0;  // SendHold::Identical — IB's 15 s identical-request rule
    uint64_t held_budget = 0;     // SendHold::Budget    — 50/10 min bulk ceiling
    uint64_t abandoned = 0;       // held past kHistQueueMaxWaitMs and errored back
};

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

    // Cumulative fetch accounting (see HistStats). Only the TWS route caches or
    // paces history, so every other source reports zeroes — a reader must treat
    // an all-zero struct as "this source does not account for it", not as "no
    // fetching happened".
    virtual HistStats hist_stats() const { return {}; }

    // True while this source is being refused its TWS API client id because
    // something already holds it (IB error 326 — see engine/tws_client_id.h).
    // It means "down, and this is why" — the source is still reconnecting, just
    // slowly, and this goes false the moment a handshake completes. Only the TWS
    // route can report it; every other source says no.
    virtual bool client_id_conflict() const { return false; }

    // Is the upstream gateway LOGGED IN to the broker, as opposed to merely
    // reachable? connected() above cannot answer that — an IB Gateway parked on
    // a failed-login dialog still accepts the socket, which is how a 13-hour
    // outage went unseen on 2026-08-09 (see net/gateway_auth.h). Proven by the
    // data farms, so only the TWS route reports it; the defaults below say "no
    // evidence" and callers must treat this as TWS-route-only.
    //   gateway_auth_age_ms: ms since the freshest proof, -1 = never.
    //   gateway_farms_ok:    how many of the three data farms are up (0..3).
    virtual bool gateway_authed() const { return false; }
    virtual int64_t gateway_auth_age_ms() const { return -1; }
    virtual int gateway_farms_ok() const { return 0; }

    // Thread-safe; return the request id used (0 if not running).
    //
    // `prio` is what the CALLER knows and the source cannot infer: whether
    // anything is blocked on the answer. The TWS route paces historical requests
    // against IB's 60-per-10-minutes ceiling, and a hold there lasts until sends
    // age out of that window — long enough that a bulk pass (the lineup's 30
    // ranking fetches) would otherwise starve a live session's warmup or a chart
    // the operator is looking at. ReqPriority::Live may draw on a small reserve
    // above the bulk ceiling; see net/hist_pacing.h. Sources that do not pace
    // ignore it.
    virtual uint32_t request_candles(const std::string& symbol,
                                     const std::string& interval,
                                     const std::string& range,
                                     ReqPriority prio = ReqPriority::Bulk) = 0;
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
