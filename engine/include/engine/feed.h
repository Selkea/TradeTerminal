#pragma once
// THE MARKET-DATA SEAM.
//
// Today, ticks arrive from the Python sidecar (delayed Yahoo quotes) via
// Engine::push_live_tick, and backtests replay cached candles. A
// professional feed (broker websocket, exchange multicast handler) plugs in
// here without touching the engine:
//
// Contract for implementers:
//  - Run your own I/O thread(s); normalize every update into an EngineEvent
//    (Tick/Bar) and push it into the engine's md_ring. One producer thread
//    per ring — if you have several sources, fan them into one thread first.
//  - Stamp ts_ingest_tsc with rdtsc() at the push site so tick-to-order
//    latency stays measurable end to end.
//  - Never block in the push path. If the ring is full, drop the tick and
//    count it (stale market data is worse than none).
//  - subscribe()/unsubscribe() are called from the UI thread; make them
//    queue work for your I/O thread rather than doing network I/O inline.

#include <cstdint>
#include <string>
#include <vector>

namespace tt {

class IFeedHandler {
public:
    virtual ~IFeedHandler() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void subscribe(const std::vector<std::string>& symbols) = 0;
    virtual void unsubscribe(const std::vector<std::string>& symbols) = 0;

    virtual bool connected() const = 0;
    virtual uint64_t dropped() const = 0;
};

} // namespace tt
