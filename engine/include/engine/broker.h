#pragma once
// THE LIVE-TRADING SEAM.
//
// Today, execution is simulated: the engine calls ExecSim directly and fills
// come back synchronously as prices arrive. To trade against a real broker
// (Alpaca, IBKR, ...), implement this interface and route EngineCtx's
// submit/cancel through it instead of ExecSim (LiveConfig::broker).
//
// Contract for implementers:
//  - submit()/cancel()/cancel_all()/flatten() are called on the engine
//    thread and must NOT block: hand the request to your own I/O thread
//    (REST/websocket/FIX) and return.
//  - Acknowledgements, fills, and rejects come back as EngineEvents through
//    poll_event(), which the engine drains every loop iteration. Produce
//    them from your I/O thread into your own SPSC ring — never call into
//    the engine synchronously.
//      Fill        -> EvType::Fill   (u.fill + symbol_id + ts_event_ns)
//      Cancelled   -> EvType::OrderCancel
//      Rejected    -> EvType::OrderCancel with kEvFlagRejected set
//  - Order ids: return your own monotonically increasing id immediately;
//    map it to the broker's id internally. Return 0 to reject client-side
//    (not connected, queue full, ...).
//  - The kill switch calls cancel_all() then flatten(); both must be safe to
//    call repeatedly and while orders are in flight.
//
// AlpacaBroker is the reference implementation.

#include "engine/events.h"
#include "tt/events.h"

#include <cstdint>
#include <string>

namespace tt {

// Why an order was rejected, captured by the adapter alongside a Rejected
// OrderCancel event. The numeric code is the broker's (e.g. IBKR 110 "price
// doesn't conform to min tick"); message is its human text. Both empty/0 when
// the reject path carried no reason (e.g. an order that just went Inactive).
struct RejectReason {
    int code = 0;
    std::string message;
};

class IBrokerAdapter {
public:
    virtual ~IBrokerAdapter() = default;

    virtual uint64_t submit(const OrderRequest& r, int64_t now_ns) = 0;
    virtual bool cancel(uint64_t order_id) = 0;
    virtual void cancel_all() = 0;
    // Close every open position at market (kill switch, after cancel_all).
    virtual void flatten() = 0;

    // Engine thread: drain pending fills/cancels/rejects. False = none left.
    virtual bool poll_event(EngineEvent& out) = 0;

    // Consume the reason for a rejected order id, if the adapter recorded one
    // when it pushed the Rejected event. Called on the engine thread as that
    // event is drained; erases the entry so it is returned at most once.
    // Default: adapters that don't track reasons return an empty reason.
    virtual RejectReason take_reject(uint64_t /*order_id*/) { return {}; }

    // True once the adapter is connected and accepting orders.
    virtual bool ready() const = 0;
};

} // namespace tt
