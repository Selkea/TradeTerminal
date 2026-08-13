#pragma once
// One place that knows what IB error 326 MEANS and how to say it to a human.
//
// WHY THIS EXISTS. On 2026-08-11 at 16:33 the operator started the GUI while a
// headless dry-run instance still held the TWS API client ids. The data client
// then logged this, every three seconds, seven times:
//
//   tws-data: connecting to IB Gateway at 127.0.0.1:4002
//   tws-data: error 326 (id -1): Unable to connect as the client id is already
//             in use. Retry with a unique client id.
//   tws-data: connection closed
//
// No session ever came up, nothing said why in words an operator could act on,
// and the incident was reported as "it crashed". The defect was the SILENCE, not
// the retry: IB's own wording is the only explanation offered, and it describes
// an API concept ("client id") the person reading the log has no reason to know.
// What they need to be told is that something else already holds the id, which
// of our clients is therefore off, and what will make it come back.
//
// 0.20.0 fixed the silence and then went too far: it also PARKED the client
// permanently, on the premise that 326 can only mean a foreign program. That
// premise is false in this app, and the repo says so in two places —
// terminal/src/app.h ("Rotates the TWS broker/feed API client ids across session
// starts so a quick stop->start never reuses an id the just-reaped connection is
// still releasing at the gateway (error 326)") and app.cpp at both id
// assignments. The gateway holds a dead socket's id for a short while after the
// process behind it goes away, so the FIRST connect after a hard kill
// (main.cpp's TerminateProcess teardown budget, a crash, a deploy) or a nightly
// gateway restart can draw a 326 that would clear on its own moments later.
// Parking turned that into: no order path and no candles for the rest of the
// trading day, with `live_running` still true and the gateway keepalive
// cold-restarting IB Gateway every four minutes forever because
// broker_connected could never come back.
//
// So: SAY IT ONCE, KEEP TRYING, QUIETLY. The latch below exists to make the
// explanation appear exactly once per episode and to slow the retry from 3 s to
// kTwsClientIdRetrySec, not to end the client. It is cleared by a successful
// handshake, which also logs the recovery — because a condition that can clear
// itself must be observably cleared, or the operator is left acting on a page
// that stopped being true.
//
// WHAT IS STILL NOT DONE: reconnecting on a DIFFERENT id. TWS scopes an API
// client's view of open orders to the id that placed them, so drifting the
// orders client would hand connect-time reconciliation a different set of
// resting orders to adopt — on the order path, on a restart. And 326 can mean a
// second instance is trading this account, which is the disaster the
// single-instance mutex exists to prevent; working around it silently is how you
// arrive there. Retrying the SAME id has neither problem: it either wins the id
// back or keeps saying it cannot.
//
// The three TWS clients (data id 9, orders 20, feed 40-59 — only the FEED id
// rotates now, see kTwsOrdersClientId) each connect independently and each can
// hit this on its own, so the constants and the sentences live here rather than
// being written out three times and drifting.
//
// ---------------------------------------------------------------------------
// 2026-08-13: THE RULE ABOVE WAS ALREADY BEING BROKEN, BY US, EVERY RESTART.
//
// The paragraph headed "WHAT IS STILL NOT DONE" was written on 2026-08-11
// reasoning about reconnect-on-a-different-id, and did not notice that
// App::start_live_session had been drifting the orders client id on every
// SESSION START since long before it — `tc.client_id = 20 + (tws_client_seq_++
// % 20)`. Restarting is not rarer than reconnecting; the daily lineup swap does
// it mid-session, on a live book.
//
// What that cost, on 2026-08-13: at 09:37:21 client 20 placed RAM's bracket
// (stop 11.67, take-profit 12.80). At 09:49:36 an automatic lineup swap tore
// that session down and rebuilt on client 22, which ADOPTED both resting exits
// out of a reqAllOpenOrders snapshot — because IB lets any client SEE every
// resting order in the account. When the 12.80 limit later filled, the
// orderStatus / execDetails / commissionReport for it went to client 20, which
// no longer existed. Nothing was dropped, nothing errored: the callbacks were
// simply delivered somewhere else. The app carried a PHANTOM 411-share position
// for 4 h 03 m, published a fictional +$528 unrealized and "protected", and a
// manual sell in that state would have opened a NAKED SHORT.
//
// The evidence, re-verified: across 30 live sessions in 15 days exactly 5
// adopted broker orders. The 3 where the adopting client id equalled the
// placing one all worked. The 2 where it differed (client 22 adopting client-20
// orders, 08-06 and 08-13) had TOTAL order-path silence — 0 acks, 0 fills. And
// on a single live order (MUU stop, TWS id 39, placed by client 20) a cancel
// from client 22 came back "error 10147: OrderId 39 that needs to be cancelled
// is not found", while the same order cancelled cleanly from client 20 twenty-
// seven minutes later. A client that cannot hear an adopted order also cannot
// cancel it.
//
// THE INVARIANT, stated once so the next edit has to argue with it: the orders
// client id names the ACCOUNT'S ORDER PATH, not this connection. Every order
// this app may ever have to hear about, cancel or replace must have been placed
// under the same id — so the id has to outlive the session, and the process.
// Rotation treated a durable identity as a per-connection nonce. That is the
// whole defect.

#include <string>

namespace tt {

// ---- the orders client id ---------------------------------------------------

// THE orders client id. One number, for this app, forever — see the 2026-08-13
// note above for what the rotating version cost.
//
// 20 rather than any other number because that is the id the rotation ALWAYS
// started from: tws_client_seq_ is a member initialised to 0, so the first
// session of every process placed on 20, and 20 is therefore the id that holds
// the account's existing resting orders across a restart. Renumbering to
// anything else would strand exactly the orders this change exists to protect,
// once, on the deploy.
//
// It is deliberately NOT a config knob. A knob here is a way to be wrong on one
// machine and right on the test box, and the failure it produces is silent.
inline constexpr int kTwsOrdersClientId = 20;

// The FEED client id still rotates (40-59), and that is not an oversight. The
// feed places no orders, so nothing is ever scoped to its id and there is
// nothing for it to adopt; rotation there buys back the "quick stop->start hits
// its own not-yet-released socket" escape hatch for free. The cost that made
// rotation wrong on the order path simply does not exist on the data path.
inline constexpr int kTwsFeedClientIdBase = 40;
inline constexpr int kTwsFeedClientIdSpan = 20;

inline int tws_feed_client_id(int seq) {
    return kTwsFeedClientIdBase + (seq % kTwsFeedClientIdSpan);
}

// IB error code: "Unable to connect as the client id is already in use."
inline constexpr int kTwsClientIdInUse = 326;

// How long a client that drew a 326 waits between connect attempts, instead of
// the ordinary 3 s.
//
// TIERED since 0.22.0, and the tier is what makes a FIXED orders id affordable.
// With rotation, a 326 on the order path meant "someone else has 20, take 22";
// with a fixed id there is nothing to take, so the retry cadence IS the
// recovery. The overwhelmingly common case is now this app's own just-reaped
// session still being released by the gateway — seconds, not minutes — and
// paying 15 s for it on every restart would mean a 15 s order-path outage with
// positions live at the broker and nothing adopted yet. So: retry fast for the
// first kTwsClientIdFastRetries attempts, then fall back to the slow cadence,
// which is the one the original rationale was about (a conflict lasting all
// afternoon costs four attempts a minute of nothing rather than twenty).
inline constexpr int kTwsClientIdRetrySec = 15;
inline constexpr int kTwsClientIdFastRetrySec = 3;
inline constexpr int kTwsClientIdFastRetries = 5;

// Seconds between connect attempts, given how many attempts this episode has
// already made. Pure so the tier is a thing a test can drive rather than an
// expression buried in io_loop.
inline int tws_client_id_retry_sec(int attempts_this_episode) {
    return attempts_this_episode < kTwsClientIdFastRetries ? kTwsClientIdFastRetrySec
                                                           : kTwsClientIdRetrySec;
}

// How long a client-id conflict must SURVIVE before it is worth paging about.
//
// The conflict tag pages Critical (alert_rules.h) and that was right when a 326
// on the order path was rare. Under a fixed id a brief 326 across a restart is
// an EXPECTED transient — our own socket, not yet released — and paging on
// every lineup swap is how you train the operator to swipe away the one line
// that mattered on 2026-08-11. So the plain-prose explanation is logged
// immediately (it is what the 2026-08-11 incident was actually about: silence),
// and the uppercase paging tag is withheld until the conflict has outlasted a
// window several fast retries long.
inline constexpr int kTwsClientIdPageAfterSec = 60;

// The token every conflict line carries, uppercase and fixed, so
// tt::ui::classify_alert can page on it without pattern-matching prose that a
// later edit would silently break. Uppercase for the same reason the lineup's
// verdicts are: ordinary log prose about clients or ids must not page anyone.
inline constexpr const char* kTwsClientIdConflictTag = "API CLIENT ID IN USE";

// The matching all-clear. A separate tag, and deliberately NOT a substring of
// the one above: the conflict pages Critical, and a recovery that pattern-
// matched into that rule would page Critical for good news.
inline constexpr const char* kTwsClientIdClearedTag = "API CLIENT ID FREE AGAIN";

// The operator-facing line, emitted ONCE per episode by the client that was
// refused. `who` names it in plain words ("market data", "the ORDER path"), NOT
// the class name; `impact` says what stops working, because "the feed client is
// off" is not a consequence anyone can act on at 16:33 on a Tuesday.
//
// BOTH causes are named. The 0.20.0 draft asserted a single diagnosis ("It is
// almost certainly a second TradeTerminal ... Close it (Task Manager), then
// restart this app"), which is exactly wrong for the case this app creates for
// itself: after a hard-killed restart the id it collides with is its OWN, no
// second tt_terminal.exe exists to close, and the remedy printed would have the
// operator kill the running trading app.
inline std::string tws_client_id_conflict_line(const char* who, const char* impact,
                                               const std::string& host, int port,
                                               int client_id) {
    return std::string(kTwsClientIdConflictTag) + ": IB Gateway at " + host + ":" +
           std::to_string(port) + " refused API client id " +
           std::to_string(client_id) +
           " because something else is already connected with it, so " + who +
           " is OFF (" + impact + "). Either a second TradeTerminal holds it - a "
           "dry run, a minimized window, a leftover tt_terminal.exe (Task "
           "Manager) - or it is this app's own previous session, whose socket "
           "the gateway has not released yet, which frees itself within a "
           "minute or two. Retrying every " + std::to_string(kTwsClientIdRetrySec) +
           "s; this line will not repeat, and \"" +
           std::string(kTwsClientIdClearedTag) + "\" will say when it recovered. "
           "If that never comes, find and close the other program (IB error " +
           std::to_string(kTwsClientIdInUse) + ").";
}

// The same explanation WITHOUT the paging tag, for the first
// kTwsClientIdPageAfterSec of an episode.
//
// It exists because the two things a 326 can mean now have very different
// urgencies and the same words. Our own socket being released across a restart
// resolves in seconds and no one needs to be woken for it; a foreign program
// holding id 20 is an all-day outage with no automatic escape, because the id
// no longer rotates. Withholding the TAG (not the sentence) for a minute
// separates them using the only evidence that actually distinguishes them —
// whether it clears — and keeps the 2026-08-11 lesson intact: the defect that
// day was SILENCE, and this still says it out loud, immediately, in the log.
inline std::string tws_client_id_waiting_line(const char* who, int client_id,
                                              int retry_sec) {
    return std::string("API client id ") + std::to_string(client_id) +
           " is still held by something else, so " + who +
           " is not connected yet - retrying every " + std::to_string(retry_sec) +
           "s. Usually this app's own previous session, released within seconds. "
           "If it lasts " + std::to_string(kTwsClientIdPageAfterSec) +
           "s this will page (IB error " + std::to_string(kTwsClientIdInUse) + ").";
}

// ---- adoption identity ------------------------------------------------------

// The tag for the one case a fixed orders id cannot rule out by construction:
// adopting a resting order that some OTHER client placed.
//
// With kTwsOrdersClientId fixed, an order placed by this app is always placed by
// client 20, so an adopted order reporting a different clientId can only have
// come from somewhere else — a build predating this change whose orders are
// still resting (the deploy itself), an order entered by hand in the Gateway
// GUI, another program on the account. In every one of those cases the order is
// VISIBLE to us and DEAF to us: reqAllOpenOrders returns it, orderStatus /
// execDetails / commissionReport for it will not, and cancelOrder answers 10147.
// That is precisely the 2026-08-13 state, and it must never again be reachable
// without someone being told.
//
// Uppercase and matched by classify_alert. The order is still adopted after
// this fires — refusing to adopt would leave a real position with nothing in the
// book at all, which is strictly worse — so the page IS the mitigation.
inline constexpr const char* kTwsForeignOrderTag = "ORDER FROM ANOTHER API CLIENT";

// `placed_by` is Order::clientId as IB reports it in openOrder (EOrderDecoder::
// decodeClientId — it is on the wire for every open order, which is what makes
// this detectable at all rather than merely arguable).
inline std::string tws_foreign_order_line(const std::string& symbol,
                                          const std::string& order_type,
                                          long tws_order_id, long placed_by,
                                          int our_client_id) {
    return std::string(kTwsForeignOrderTag) + ": resting " + order_type + " on " +
           symbol + " (TWS order " + std::to_string(tws_order_id) +
           ") was placed by API client " + std::to_string(placed_by) +
           ", but this session is client " + std::to_string(our_client_id) +
           ". IB scopes fills and cancels to the client that PLACED an order, so "
           "this app will NOT be told when it fills and CANNOT cancel it (error "
           "10147). It is adopted into the book anyway - dropping it would hide a "
           "real position - but the book's copy can go stale without warning: on "
           "2026-08-13 exactly this carried a phantom 411-share position for 4h. "
           "Watch book_divergence in /diag, and flatten from the Gateway GUI if "
           "it needs to go.";
}

// ---- order id exhaustion (IB error 103) -------------------------------------

// "Duplicate order id". Mechanism B of the 2026-08-13 incident: next_tws_id is
// seeded only from nextValidId and was never advanced past the ids of the orders
// reconciliation had just ADOPTED, so a session could start below the account's
// high-water mark (seed 59 on 08-13 while the placing client had reached ~76)
// and every single placement collided.
//
// It has to page. 103 is not in fatal_order_error, so a rejected placement
// pushed no reject event: the engine left the order Working forever and only
// check_stuck muttered after 15 s. The operator's manual sell that afternoon was
// refused this way — which is, blackly, the only reason the phantom position did
// not turn into a naked short — and nothing anywhere said the order path was
// paralysed.
inline constexpr int kTwsDuplicateOrderId = 103;
inline constexpr const char* kTwsDuplicateOrderIdTag = "DUPLICATE ORDER ID";

inline std::string tws_duplicate_order_id_line(long tws_order_id, long next_id,
                                               int our_client_id) {
    return std::string(kTwsDuplicateOrderIdTag) + ": IB refused TWS order id " +
           std::to_string(tws_order_id) + " on client " +
           std::to_string(our_client_id) +
           " because that id is already in use on this account, so the order "
           "NEVER REACHED THE MARKET. The id counter has been advanced to " +
           std::to_string(next_id) +
           " and the order rejected so the engine stops waiting on it. If this "
           "repeats, every placement is being refused and the order path is "
           "paralysed - the state that hid a phantom position for four hours on "
           "2026-08-13 (IB error " + std::to_string(kTwsDuplicateOrderId) + ").";
}

// The account-wide high-water mark this session must not seed below.
//
// Pure, so the one rule that matters is testable without a gateway: the next id
// only ever moves FORWARD. Three callers, all of which could otherwise move it
// backwards — nextValidId on a mid-session reconnect (the server's seed can be
// lower than ids this session has already spent), adoption (an adopted order's
// id can exceed the seed), and the 103 self-heal.
inline long tws_advance_order_id(long current, long candidate) {
    return candidate > current ? candidate : current;
}

// The all-clear, emitted once by the same client when the handshake finally
// completes. Its whole job is to end the Critical above: an operator paged at
// 16:33 must be able to tell, from the log alone, whether they still have to do
// anything.
inline std::string tws_client_id_cleared_line(const char* who, int client_id) {
    return std::string(kTwsClientIdClearedTag) + ": API client id " +
           std::to_string(client_id) + " was released and " + who +
           " is connected again - no action needed (IB error " +
           std::to_string(kTwsClientIdInUse) + " cleared).";
}

} // namespace tt
