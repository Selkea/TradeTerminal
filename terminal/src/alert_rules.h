#pragma once
// Push-alert classification for a log line, kept as a pure function separate
// from AlertNotifier's beep/webhook side effects so the rules are unit-testable
// and there's ONE place that decides what pages the phone.

#include "engine/reject.h"          // kOrderRefusedTag and friends
#include "engine/tws_client_id.h"   // kTwsClientIdConflictTag
#include "net/book_divergence.h"    // kBookDivergenceTag
#include "symbol_params.h"          // kUnreachableStopTag

#include <string>

namespace tt::ui {

enum class AlertClass { None, Info, Warning, Critical };

inline AlertClass classify_alert(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    if (has("KILL SWITCH") || has("RISK HALT") || has("WATCHDOG") ||
        has("PROTECTIVE STOP REJECTED") || has("EOD BACKSTOP"))
        return AlertClass::Critical;
    // EOD BACKSTOP is on that list as of 0.29.3, and was AlertClass::None for its
    // whole life. It is the app's LAST RESORT: reaching it means every
    // strategy-level exit failed and the engine had to cancel-all and force-flatten
    // a position it should never still have been holding (engine.cpp, both the
    // broker and the sim branch). It sat one tier below "lineup: EXCLUDED" and level
    // with ordinary prose. If it ever starts firing daily, that frequency is itself
    // the alarm and must not be silenced by demoting it again.
    // A TWS client-id collision (IB error 326). Critical rather than Warning
    // because while it lasts it is TOTAL for the client that drew it: on the
    // data client there are no candles — no charts, no warmup, no lineup — and
    // on the orders client nothing can reach the market. On 2026-08-11 it
    // produced a three-line stanza every 3 s and nothing else, and was reported
    // to the operator's own eyes as "it crashed".
    //
    // It may still self-heal — the id is often our own just-reaped session's,
    // which the gateway releases on its own — so it pages once and the client
    // keeps retrying. The all-clear below is what closes the episode; it is
    // deliberately a different tag, so recovering cannot page Critical.
    if (has(kTwsClientIdConflictTag)) return AlertClass::Critical;
    if (has(kTwsClientIdClearedTag)) return AlertClass::Warning;
    // A resting order placed by some OTHER API client, adopted anyway. IB routes
    // fills and cancels to the PLACING client, so the book's copy of this order
    // is deaf: it can fill without the app ever hearing, which is exactly the
    // 4-hour phantom position of 2026-08-13. Critical because nothing about the
    // state announces itself afterwards — the order simply looks resting forever.
    if (has(kTwsForeignOrderTag)) return AlertClass::Critical;
    // Every placement refused because its id is already in use on the account:
    // the order path is paralysed and, before 0.22.0, silently so.
    if (has(kTwsDuplicateOrderIdTag)) return AlertClass::Critical;
    // A live position that the broker and the app disagree about. One of the two
    // is lying and code cannot tell which, so this is the loudest thing the app
    // can say and it deliberately does nothing else. See net/book_divergence.h.
    if (has(net::kBookDivergenceTag)) return AlertClass::Critical;
    if (has(net::kBookAuditBlindTag)) return AlertClass::Critical;
    // A symbol whose only exit cannot fire inside a trading day. Critical, and
    // in the same tier as a rejected protective stop, because it describes the
    // same state: a position with nothing that will close it at a loss.
    // bollinger_reversion and rsi2_pullback place NO price stop by design, so
    // their time stop is the whole risk control — and on 2026-08-14 STKH's was
    // fitted to 233 bars of 300 s against a 77-bar day. The position lost $506
    // in 2 h 16 m and only the 15:57 engine backstop ever closed it. Nothing
    // paged; the app's naked-position alert deliberately exempts these two
    // strategies precisely because they are SUPPOSED to hold without a stop.
    // That exemption is only sound while the time stop is reachable, which is
    // what this line now says out loud.
    if (has(kUnreachableStopTag)) return AlertClass::Critical;
    // An off-lineup broker position found at reconciliation: real stock this
    // session does not trade, cannot audit, and cannot flatten. 20 NVDA shares
    // sat in the account from 2026-07-21 to 2026-08-13 because reconciliation
    // discarded the row without even logging it (engine/reconcile_policy.h).
    if (has("OFF-LINEUP BROKER POSITION")) return AlertClass::Critical;
    // An order that would have REDUCED or CLOSED a position was refused. Critical
    // on the first occurrence and with no repeat threshold, because there is no
    // benign version: the position it would have closed is still open and one
    // fewer thing is now watching it. This is the same class the tier already
    // holds "PROTECTIVE STOP REJECTED" for, widened to cover a plain signal exit
    // — the protective tag only covers stop legs the ADAPTER knew to mark, and a
    // strategy's own market exit refused by a risk gate is just as naked.
    // Checked before the two below: all three tags contain "ORDER REFUSED".
    if (has(kExitOrderRefusedTag)) return AlertClass::Critical;
    // NB: match "half-open ORDER" (a broker order submitted but never acked),
    // NOT bare "half-open". The routine data-feed reconnect logs "...data
    // session (half-open)" — it self-heals on every nightly gateway restart and
    // flaps all weekend, and was spamming the phone with Warning pages.
    // A refused trading day is the loudest thing the lineup can say and it used
    // to be unobservable: before 0.16.0 the lineup always started SOMETHING, so
    // "no session today" was not a reachable outcome. Now it is, and an operator
    // who is not at the machine has to hear it. Uppercase on purpose — the
    // lineup emits these two verdicts in caps, and "excluded"/"aborted" in
    // ordinary prose must not page anyone.
    if (has("lineup: ABORTED")) return AlertClass::Critical;
    if (has("lineup: EXCLUDED")) return AlertClass::Warning;
    // The SAME symbol refused for the SAME reason, kRefusalRepeatAlertAt times.
    // One refusal is a decision the engine made on purpose; a repeat is a
    // strategy stuck in a loop it cannot leave, which means that symbol is
    // silently not trading and will not start on its own. Beep-worthy, not
    // page-the-whole-tier worthy. Must precede the plain tag below, which it
    // contains as a substring.
    if (has(kOrderRefusedRepeatTag)) return AlertClass::Warning;
    // A single refused ENTRY. Info — webhook, no beep. Most refusals are the
    // system working as designed (the notional clamp finding a position already
    // at its cap; the entry gate declining to buy into a shut exchange), and a
    // beep for each would train the operator to ignore beeps. That is not a
    // hypothetical: the "half-open" data-feed line above was de-escalated for
    // exactly that reason. It is still on the channel, because a refusal the
    // operator never hears about is what let a strategy signal into a closed
    // exchange three times on 2026-08-13 with nothing said.
    //
    // MUST precede the generic "rejected" rule below, not follow it. The line
    // embeds reject_cause_slug(), and the slug for a broker-caused refusal is
    // "broker_rejected" — which contains "rejected". Below the generic rule,
    // every refusal IB caused (the 201 "Exchange is closed" of 2026-08-13, all
    // six lineup symbols) beeped Warning while the policy above called it Info,
    // and the suite missed it because its cases all used local slugs.
    if (has(kOrderRefusedTag)) return AlertClass::Info;
    // The engine's own trace line for a broker rejection it has ALREADY logged,
    // one line earlier, with one of the tags above (engine.cpp's broker-event
    // drain). It repeats the broker's own text — "Order rejected -
    // reason:Exchange is closed." — so without this it matched the generic rule
    // below and paged a SECOND time, at Warning, for exactly the refused
    // entries the tagged line rates Info. The tag decides the tier; this line
    // exists to carry the order id and the broker's numeric code into the log.
    if (has("refused by broker")) return AlertClass::None;
    // The TWS adapter's raw trace of an IB error callback — see kIbErrorTraceTag
    // for why it is a trace and not the report. None, exactly like the "refused
    // by broker" line above it and for the same reason: every IB error worth
    // hearing about already has a louder path, and they are all checked above.
    // An order refusal has its tagged ORDER REFUSED line (tiered AND throttled);
    // a client-id collision, a duplicate order id and a foreign order have their
    // own tags; a sustained loss of the gateway's upstream link is raised as
    // STATE by pump_broker_watchdog, which pages Critical after 60 s rather than
    // on each callback.
    //
    // It was briefly Info instead, on the theory that a non order-scoped IB
    // error has no other report. The VPS log says otherwise: one ordinary
    // overnight IBC restart produces six 1100s in 42 seconds and a pair of 502s,
    // all of them routine, all of them silent before this tag existed. Info
    // still posts to the webhook, so that reasoning would have added phone
    // traffic to a nightly maintenance window while fixing a flood — and the
    // 60 s watchdog is what tells the operator when one of those restarts does
    // NOT come back.
    if (has(kIbErrorTraceTag)) return AlertClass::None;
    // A STRATEGY'S OWN FREE TEXT, tiered by the LEVEL the strategy chose rather
    // than by keyword. EngineCtx::log stamps the level into the prefix, and that
    // is a far better signal than scanning prose written by six different
    // strategy authors — which is what happened until 2026-08-17.
    //
    // Both directions were wrong, in opposite ways:
    //
    //  - donchian_trend's level-2 `ctx.log(2, "entry rejected")` contains
    //    "rejected", so it fell through to the generic rule below and paged
    //    WARNING — walking straight around the Info tier the rule above builds
    //    for refusals, and for the reason stated there: a beep per refusal
    //    trains the operator to ignore beeps. It sent 73 pages in 90 seconds.
    //  - meanwhile every strategy's level-3 "protective stop rejected —
    //    position unprotected" ALSO landed on that generic rule, at the same
    //    Warning, because the Critical tag at the top is UPPERCASE and matched
    //    only the engine's own spelling (engine.cpp). A strategy reporting a
    //    naked position was rated exactly as loud as one declining to buy.
    //
    // So level 3 is Critical — it is what a strategy uses to say that something
    // which should be protecting or closing a position is not ("protective stop
    // rejected", "EOD flatten died — position still open"), which is precisely
    // the tier's definition. Level 2 is Info: on the channel, no beep. The
    // engine's own tagged lines are checked ABOVE and still outrank both, so
    // this cannot downgrade a refusal the engine classified itself.
    if (has("[strategy error]")) return AlertClass::Critical;
    if (has("[strategy warn]")) return AlertClass::Info;
    if (has("rejected") || has("stream lost") || has("auth failed") ||
        has("(drops!)") || has("half-open order"))
        return AlertClass::Warning;
    // A LIVE FILL. Info: on the channel, no beep.
    //
    // This tested has("live: fill") until 0.29.3 and had therefore matched NOTHING
    // since 0.4.3 (f93b0c3, "name the symbol in log lines"), which rewrote the
    // emitter to "live: %s fill %s %.0f @ %.2f (order #%llu)" — the SYMBOL is
    // interpolated between "live: " and "fill", so the substring never occurs and
    // every fill silently fell through to None. Matched on two fragments that
    // straddle the interpolation instead, so the same edit cannot break it twice.
    if (has(" fill ") && has("(order #"))
        return AlertClass::Info;
    return AlertClass::None;
}


// ---------------------------------------------------------------------------
// WHAT KIND of event this is, independent of how loud it is.
//
// classify_alert answers "how urgent"; this answers "about what". They are
// different questions and the operator wants to silence along the second axis:
// "stop paging me about fills" must not also silence a naked position, and
// muting the whole channel (the old Alerts toggle) was the only control there
// was — all or nothing, which is why it was never used.
//
// Kept next to classify_alert, and in the SAME ORDER, so one line cannot be
// urgent-because-of-tag-X but categorised-by-tag-Y. Anything unrecognised is
// System, never silently Trades.
enum class AlertCategory {
    Trades = 0,      // fills: the strategy did something
    Orders,          // an order refused or rejected before/at the broker
    Risk,            // halts, kill switch, EOD backstop, a position with no exit
    Connection,      // gateway, broker socket, data feed, client-id collisions
    Integrity,       // the app and the broker disagree about what is held
    Lineup,          // the daily build, and the session guard that ends the day
    System,          // everything else
    COUNT
};

inline constexpr const char* alert_category_name(AlertCategory c) {
    switch (c) {
    case AlertCategory::Trades:     return "Trades";
    case AlertCategory::Orders:     return "Orders";
    case AlertCategory::Risk:       return "Risk";
    case AlertCategory::Connection: return "Connection";
    case AlertCategory::Integrity:  return "Integrity";
    case AlertCategory::Lineup:     return "Lineup";
    default:                        return "System";
    }
}

// True for the categories that report a position nothing is protecting or a book
// nobody can trust. They are toggleable like the rest — it is the operator's
// terminal — but the UI says out loud what turning them off costs, and every
// suppressed page is still counted in /diag. Silence that no one chose is the
// failure this whole subsystem exists to prevent; silence someone DID choose
// still has to be visible.
inline constexpr bool alert_category_is_safety(AlertCategory c) {
    return c == AlertCategory::Risk || c == AlertCategory::Integrity;
}

inline AlertCategory classify_alert_category(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    // RISK first, for the same reason classify_alert checks Critical first: a
    // line that is both a fill and a forced flatten is about the flatten.
    if (has("KILL SWITCH") || has("RISK HALT") || has("EOD BACKSTOP") ||
        has("PROTECTIVE STOP REJECTED") || has(kUnreachableStopTag) ||
        has("unprotected") || has("orphaned position") ||
        // A refused EXIT is Risk, NOT Orders. "Orders" is the switch someone
        // flips to stop hearing about routine refusals, and this tag nests
        // inside kOrderRefusedTag ("EXIT ORDER REFUSED" contains "ORDER
        // REFUSED") — so without this line it fell through to Orders and a
        // position left open with nothing watching it became silenceable by
        // the category for the most routine event there is. Same substring
        // trap classify_alert documents for the same three tags.
        has(kExitOrderRefusedTag))
        return AlertCategory::Risk;
    if (has(net::kBookDivergenceTag) || has(net::kBookAuditBlindTag) ||
        has("OFF-LINEUP BROKER POSITION") || has(kTwsForeignOrderTag) ||
        has("half-open order"))
        return AlertCategory::Integrity;
    // A duplicate order id paralyses the ORDER path — it is a broker-session
    // fault, not a decision about one order.
    if (has(kTwsClientIdConflictTag) || has(kTwsClientIdClearedTag) ||
        has(kTwsDuplicateOrderIdTag) || has("stream lost") || has("auth failed") ||
        has("disconnected") || has("reconnected") || has("gateway") ||
        has("stale candles"))
        return AlertCategory::Connection;
    if (has("lineup:") || has("session guard")) return AlertCategory::Lineup;
    if (has(kOrderRefusedTag) || has("rejected") || has("refused"))
        return AlertCategory::Orders;
    if (has(" fill ") && has("(order #")) return AlertCategory::Trades;
    return AlertCategory::System;
}

} // namespace tt::ui
