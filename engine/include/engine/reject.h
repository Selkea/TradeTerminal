#pragma once
// WHY AN ORDER WAS REFUSED - an enum, a slug, and a sentence. Never blank.
//
// 2026-08-13, 16:00:12 and 16:00:18 ET: /diag reported reject_count 2, and both
// entries in rejects_recent carried reject_code 0 with reject_msg "". The orders
// had in fact been refused by IBKR with error 201 "Order rejected -
// reason:Exchange is closed." - the text reached terminal.log and NOTHING else,
// because TwsBroker::orderStatus("Inactive") won a race against
// TwsBroker::error(201) for a shared suppression set and pushed a reason-less
// reject event (see tws_broker.cpp, pending_inactive). Reading /diag, the
// operator could not tell a local risk check from a buying-power gate from a
// notional clamp from something IB said. It was the third time that page had
// been unreadable: SNXX lost its reason the same way on 2026-08-11.
//
// THE RULE THIS HEADER EXISTS TO ENFORCE: an OrderRecord with status ==
// Rejected carries cause != None AND a non-empty message. Every `return 0` in
// EngineCtx::submit_order names its cause; every adapter that returns 0 from
// submit() answers last_submit_reject(); and the IBrokerAdapter defaults are
// deliberately non-empty, so an adapter that forgets still says something an
// operator can act on rather than nothing at all.
//
// The enum is the greppable, testable half (reject_cause_slug -> "session_closed")
// and the sentence is the operator's half. kAllRejectCauses walks every
// enumerator so a test can prove there are no gaps - engine/tests/test_reject.cpp
// fails the build's test step if an enumerator is ever added without both.

#include <cstddef>
#include <cstdint>

namespace tt {

// Ordered from "the request was malformed" through the local risk gates to the
// broker's own refusals. Contiguous from None = 0: kAllRejectCauses and Count_
// both depend on that, and the test asserts it.
enum class RejectCause : uint16_t {
    None = 0,
    // ---- the request never had a chance ----
    InvalidQty,           // qty <= 0
    SymbolNotInSession,   // symbol_id outside the running session's symbol list
    MissingLimitPrice,    // Limit order with limit_price <= 0
    MissingStopPrice,     // Stop order with stop_price <= 0
    WarmupReplay,         // signalled while replaying seed history: bar is historical
    // ---- local risk gates (EngineCtx) ----
    MaxOrderQty,
    MaxPositionQty,
    PriceBand,
    NotionalCap,
    PriceUnavailable,     // no usable price to size/value the order against
    BuyingPower,
    HoldBlocksLoss,       // hold-until-profitable refused a losing exit
    SessionClosed,        // outside the window in which an entry may be placed
    StrategyHalted,       // this symbol (or the session) is quarantined
    // ---- the execution venue said no ----
    SimRefused,           // ExecSim returned 0 (should be unreachable: pre-screened)
    BrokerNotConnected,
    BrokerReadOnly,
    BrokerQueueFull,
    BrokerUnknownSymbol,
    BrokerRefused,        // adapter returned 0 / rejected with nothing more specific
    BrokerRejected,       // accepted, then refused - carries the broker's numeric code
    Count_                // walk sentinel; never stored
};

// Every stored enumerator, in declaration order. THE list a test walks to prove
// the mapping below has no holes. Keep in sync with the enum - the test asserts
// size == Count_ and catches a forgotten entry.
inline constexpr RejectCause kAllRejectCauses[] = {
    RejectCause::None,
    RejectCause::InvalidQty,
    RejectCause::SymbolNotInSession,
    RejectCause::MissingLimitPrice,
    RejectCause::MissingStopPrice,
    RejectCause::WarmupReplay,
    RejectCause::MaxOrderQty,
    RejectCause::MaxPositionQty,
    RejectCause::PriceBand,
    RejectCause::NotionalCap,
    RejectCause::PriceUnavailable,
    RejectCause::BuyingPower,
    RejectCause::HoldBlocksLoss,
    RejectCause::SessionClosed,
    RejectCause::StrategyHalted,
    RejectCause::SimRefused,
    RejectCause::BrokerNotConnected,
    RejectCause::BrokerReadOnly,
    RejectCause::BrokerQueueFull,
    RejectCause::BrokerUnknownSymbol,
    RejectCause::BrokerRefused,
    RejectCause::BrokerRejected,
};

// Machine half: a stable lower_snake slug for /diag, Prometheus labels and
// `grep`. Never localise, never reword - these are identifiers. "" for a value
// outside the enum, which the test treats as a gap.
constexpr const char* reject_cause_slug(RejectCause c) noexcept {
    switch (c) {
    case RejectCause::None:                return "none";
    case RejectCause::InvalidQty:          return "invalid_qty";
    case RejectCause::SymbolNotInSession:  return "symbol_not_in_session";
    case RejectCause::MissingLimitPrice:   return "missing_limit_price";
    case RejectCause::MissingStopPrice:    return "missing_stop_price";
    case RejectCause::WarmupReplay:        return "warmup_replay";
    case RejectCause::MaxOrderQty:         return "max_order_qty";
    case RejectCause::MaxPositionQty:      return "max_position_qty";
    case RejectCause::PriceBand:           return "price_band";
    case RejectCause::NotionalCap:         return "notional_cap";
    case RejectCause::PriceUnavailable:    return "price_unavailable";
    case RejectCause::BuyingPower:         return "buying_power";
    case RejectCause::HoldBlocksLoss:      return "hold_blocks_loss";
    case RejectCause::SessionClosed:       return "session_closed";
    case RejectCause::StrategyHalted:      return "strategy_halted";
    case RejectCause::SimRefused:          return "sim_refused";
    case RejectCause::BrokerNotConnected:  return "broker_not_connected";
    case RejectCause::BrokerReadOnly:      return "broker_read_only";
    case RejectCause::BrokerQueueFull:     return "broker_queue_full";
    case RejectCause::BrokerUnknownSymbol: return "broker_unknown_symbol";
    case RejectCause::BrokerRefused:       return "broker_refused";
    case RejectCause::BrokerRejected:      return "broker_rejected";
    case RejectCause::Count_:              break;
    }
    return "";
}

// Operator half: one sentence that says what happened AND what it implies, so a
// page is actionable without opening the source. Pure ASCII - AlertNotifier
// folds non-ASCII to '-' before POSTing, and ntfy serves a non-ASCII body as a
// download instead of a message (see terminal/src/alerts.h).
constexpr const char* reject_cause_text(RejectCause c) noexcept {
    switch (c) {
    case RejectCause::None:
        return "no cause recorded - this is a bug, see engine/reject.h";
    case RejectCause::InvalidQty:
        return "the order quantity was zero or negative";
    case RejectCause::SymbolNotInSession:
        return "the symbol is not one this live session is running";
    case RejectCause::MissingLimitPrice:
        return "a limit order was submitted without a limit price";
    case RejectCause::MissingStopPrice:
        return "a stop order was submitted without a trigger price";
    case RejectCause::WarmupReplay:
        return "the strategy signalled while replaying seed history, so the bar "
               "is historical and nothing was placed";
    case RejectCause::MaxOrderQty:
        return "the order quantity exceeds the per-order share limit";
    case RejectCause::MaxPositionQty:
        return "the resulting position would exceed the per-symbol share limit";
    case RejectCause::PriceBand:
        return "the limit price is outside the band around the last trade "
               "(fat finger?)";
    case RejectCause::NotionalCap:
        return "the position is already at its dollar cap, so not one more "
               "share fits";
    case RejectCause::PriceUnavailable:
        return "there is no usable price for this symbol, so the order cannot "
               "be sized against the dollar cap - refused rather than sent "
               "unsized";
    case RejectCause::BuyingPower:
        return "the simulated account cannot cover this order";
    case RejectCause::HoldBlocksLoss:
        return "hold-until-profitable mode is on and this exit would realise a "
               "loss, so the position is being held instead";
    case RejectCause::SessionClosed:
        return "the exchange is not open for new entries right now, so the "
               "order would be refused or rest until the next open";
    case RejectCause::StrategyHalted:
        return "this symbol is halted, so its strategy may not place orders";
    case RejectCause::SimRefused:
        return "the fill simulator refused the order";
    case RejectCause::BrokerNotConnected:
        return "the broker order path is not connected, so nothing can reach "
               "the market";
    case RejectCause::BrokerReadOnly:
        return "the account is READ-ONLY, so trading is disabled";
    case RejectCause::BrokerQueueFull:
        return "the broker command queue is full and the order was dropped";
    case RejectCause::BrokerUnknownSymbol:
        return "the broker adapter does not know this symbol";
    case RejectCause::BrokerRefused:
        return "the broker refused the order and gave no reason";
    case RejectCause::BrokerRejected:
        return "the broker rejected the order";
    case RejectCause::Count_:
        break;
    }
    return "";
}

// True for the causes that mean "the venue/adapter said no", as opposed to a
// local gate this app chose to apply. Splits an operator question that /diag
// could not answer at all on 2026-08-13: is this us, or is this them?
constexpr bool reject_cause_is_broker(RejectCause c) noexcept {
    switch (c) {
    case RejectCause::BrokerNotConnected:
    case RejectCause::BrokerReadOnly:
    case RejectCause::BrokerQueueFull:
    case RejectCause::BrokerUnknownSymbol:
    case RejectCause::BrokerRefused:
    case RejectCause::BrokerRejected:
        return true;
    default:
        return false;
    }
}

// ---- alert tags -------------------------------------------------------------
// Matched verbatim by tt::ui::classify_alert (terminal/src/alert_rules.h). They
// live here, next to the enum, so the log line and the classifier cannot drift:
// a rejection that stopped paging because someone reworded a log string is the
// same failure class as one that stopped explaining itself.
//
// Uppercase on purpose, like every other tag that pages - "refused" in ordinary
// prose (the lineup, the data feed) must not reach anyone's phone.
inline constexpr const char* kOrderRefusedTag = "ORDER REFUSED";
inline constexpr const char* kOrderRefusedRepeatTag = "ORDER REFUSED REPEATEDLY";
inline constexpr const char* kExitOrderRefusedTag = "EXIT ORDER REFUSED";

// How many times one (symbol, cause) pair may be refused before it stops being a
// decision and starts being a strategy stuck in a loop. 3, not 2, because a
// single bar can legitimately produce an entry plus its bracket leg and have
// both hit the same gate.
inline constexpr uint64_t kRefusalRepeatAlertAt = 3;

} // namespace tt
