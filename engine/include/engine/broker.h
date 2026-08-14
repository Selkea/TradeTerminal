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
#include "engine/reject.h"
#include "tt/events.h"

#include <cstdint>
#include <string>

namespace tt {

// Why an order was rejected, captured by the adapter alongside a Rejected
// OrderCancel event. `cause` is the machine-readable taxonomy (engine/reject.h);
// `code` is the broker's own number when it gave one (e.g. IBKR 201 "Exchange is
// closed", 110 "price doesn't conform to min tick"); `message` is the human text.
//
// A default-constructed RejectReason means "the adapter recorded nothing", which
// is a state the engine REPLACES rather than stores: on 2026-08-13 two orders
// reached /diag as code 0 with an empty message and the operator had no way to
// tell what had refused them. See EngineCtx / run_live's OrderCancel handling.
struct RejectReason {
    RejectCause cause = RejectCause::None;
    int code = 0;
    std::string message;

    bool empty() const { return cause == RejectCause::None && message.empty(); }
};

// The generic non-empty reason. Used wherever an adapter refused an order
// without saying why: it is still a poor answer, but it is an answer, and it
// names the layer that produced it so the next reader knows where to look.
inline RejectReason unexplained_broker_reject() {
    return {RejectCause::BrokerRefused, 0,
            reject_cause_text(RejectCause::BrokerRefused)};
}

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
    //
    // Default: a GENERIC BUT NON-EMPTY reason, not a blank one. An adapter that
    // does not track reasons still owes the operator a sentence - the blank that
    // used to come back here is what made the 2026-08-13 rejects unreadable.
    virtual RejectReason take_reject(uint64_t /*order_id*/) {
        return unexplained_broker_reject();
    }

    // Why the most recent submit() returned 0. Called on the engine thread
    // immediately after that 0 and before any other submit on this adapter, so
    // an implementation may keep it in a plain member (submit() is documented
    // above as engine-thread-only).
    //
    // Default: the same generic non-empty reason. A submit that returns 0 with
    // nothing to say is the same defect class as a reject with no message -
    // /diag shows "an order was refused" and the operator is left guessing
    // between "not connected", "read-only account" and "queue full", which have
    // completely different responses.
    virtual RejectReason last_submit_reject() const {
        return unexplained_broker_reject();
    }

    // True once the adapter is connected and accepting orders.
    virtual bool ready() const = 0;

    // True if this adapter replays the account's existing positions, resting
    // orders, and cash on connect (as PosSnap/AcctSnap/OrderNew events followed
    // by ReconcileEnd) so a restarted session can adopt them instead of starting
    // flat. Default false: the engine then never gates dispatch on a reconcile
    // that will never come (paper/sim and the web adapter behave as before).
    virtual bool reconciles() const { return false; }
};

} // namespace tt
