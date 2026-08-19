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
    HoldOnlySymbol,       // carried only to hold/close a position, never to open one
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
    RejectCause::HoldOnlySymbol,
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
    case RejectCause::HoldOnlySymbol:      return "hold_only_symbol";
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
    case RejectCause::HoldOnlySymbol:
        return "this symbol is carried only so its existing position can be "
               "adopted, audited and closed - it may reduce or exit, never open";
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

// True for the two causes that refuse an EXIT by DESIGN. Everything that counts
// refused exits - /diag's refused_exits, the tt_refused_exits metric, any
// alerting rule built on either - must skip these, because both fields are
// documented "there is no benign value but 0" and these two are benign every
// single time:
//
//   hold_blocks_loss  IS hold-until-profitable. The strategy re-offers its exit
//                     every bar and the mode declines it until the position is
//                     green; one held position produced 189 of them in a
//                     three-second test.
//   warmup_replay     the strategy is being shown seed history and nothing was
//                     ever meant to reach the market. A HEALTHY session has one
//                     such row per symbol (see RefusalStat).
//
// Neither is paged by run_live either - note_refusal returns before the exit
// branch for both - so this keeps the gauge and the pager saying the same thing.
constexpr bool reject_cause_exit_is_by_design(RejectCause c) noexcept {
    return c == RejectCause::HoldBlocksLoss || c == RejectCause::WarmupReplay;
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

// The TWS adapter's RAW TRACE of an IB error callback (tws_broker.cpp). Every
// IB error that is not data-farm status noise passes through it, carrying IB's
// own text — and for a refused order that text is "Order rejected -
// reason:Exchange is closed.", which contains "rejected" and so fell through to
// classify_alert's generic keyword rule and paged WARNING.
//
// That is a THIRD copy of one event, at a tier the policy explicitly rejects.
// The same refusal already produces the tagged line above (Info, and throttled
// by note_refusal's repeat counter) and the engine's "refused by broker" trace
// (silent, for exactly this reason). On 2026-08-13 all six lineup symbols
// signalled into a closed exchange and each one paged here, unthrottled.
//
// Tagging it lets the classifier tier it as what it is: a trace, kept in the
// log and off the phone. Every IB error worth paging for already has a louder
// path — the tagged refusal lines here, the client-id and duplicate-id tags, and
// pump_broker_watchdog for a sustained upstream loss — and all of them are
// checked above it.
//
// UPPERCASE for the same reason as the tags above, and it was not academic: the
// first cut used "IB error", which is exactly how tws_client_id_waiting_line
// spells it in ordinary prose ("...this will page (IB error 326)."). That line
// is deliberately SILENT — the 2026-08-11 lesson was to explain a brief
// collision immediately without paging for it — and the lowercase tag quietly
// promoted it onto the channel. Caught by the test that pins it.
inline constexpr const char* kIbErrorTraceTag = "IB ERROR";

// How many times one (symbol, cause) pair may be refused before it stops being a
// decision and starts being a strategy stuck in a loop. 3, not 2, because a
// single bar can legitimately produce an entry plus its bracket leg and have
// both hit the same gate.
//
// Compared with >=, never ==. The counter is incremented for EVERY refusal of
// the pair, including the ones that return before this check is reached (an
// exit, a hold_blocks_loss); an == test lets those consume the value 3 and the
// Warning then never fires for that pair again, no matter how many times the
// strategy jams. A pair that has already warned re-arms at count * 10, so a
// session that keeps failing says so again at 30, 300, ... rather than once.
inline constexpr uint64_t kRefusalRepeatAlertAt = 3;

// A refused EXIT pages Critical on the FIRST occurrence and keeps saying so
// while it keeps happening. Both halves are load-bearing:
//
//  * One page is not enough. Until 0.23.1 the Critical tag was emitted only on
//    the first refusal of a (symbol, cause) pair, which meant a refused exit was
//    SILENT whenever an entry had already been refused for the same cause on
//    that symbol earlier in the session — and after that, permanently silent.
//    The position it would have closed is still open the whole time.
//  * A page per refusal is too much. A strategy re-offers its exit every tick,
//    so a 12-hour gateway outage (2026-07-30) with an open position would have
//    produced a Critical every few milliseconds and muted the channel — the
//    exact failure the "half-open" data-feed spam taught this project.
//
// So it repeats with a geometric backoff: immediately, then 5 min later, then
// x3 each time up to an hour. Loud while an operator can still act, never
// silent, bounded at ~8 pages over a 12-hour outage per (symbol, cause).
inline constexpr int64_t kExitRefusalRepageNs = 300LL * 1'000'000'000;      // 5 min
inline constexpr int64_t kExitRefusalRepageMaxNs = 3600LL * 1'000'000'000;  // 1 h
inline constexpr int64_t kExitRefusalRepageGrowth = 3;

} // namespace tt
