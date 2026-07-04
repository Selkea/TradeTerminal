#pragma once
// THE LIVE-TRADING SEAM.
//
// Today, execution is simulated: the engine calls ExecSim directly and fills
// come back synchronously as prices arrive. To trade against a real broker
// (Alpaca, IBKR, ...), implement this interface and route EngineCtx's
// submit/cancel through it instead of ExecSim.
//
// Contract for implementers:
//  - submit()/cancel() are called on the engine thread and must NOT block:
//    hand the request to your own I/O thread (REST/websocket/FIX) and return.
//  - Acknowledgements, fills, and rejects come back by pushing OrderEvents
//    into the engine's md_ring (or a dedicated broker ring) — never by
//    calling into the engine synchronously from your I/O thread.
//  - Order ids: return your own monotonically increasing id immediately;
//    map it to the broker's id internally.
//  - The kill switch calls cancel_all() then flatten(); both must be safe to
//    call repeatedly and while orders are in flight.
//
// PaperBroker (the ExecSim wrapper) is the reference implementation shape.

#include "tt/events.h"

#include <cstdint>

namespace tt {

class IBrokerAdapter {
public:
    virtual ~IBrokerAdapter() = default;

    virtual uint64_t submit(const OrderRequest& r, int64_t now_ns) = 0;
    virtual bool cancel(uint64_t order_id) = 0;
    virtual void cancel_all() = 0;

    // True once the adapter is connected and accepting orders.
    virtual bool ready() const = 0;
};

} // namespace tt
