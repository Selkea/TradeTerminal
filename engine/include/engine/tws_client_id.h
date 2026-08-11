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
// The three TWS clients (data id 9, orders 20-39, feed 40-59 — only the data id
// is fixed) each connect independently and each can hit this on its own, so the
// constants and the sentences live here rather than being written out three
// times and drifting.

#include <string>

namespace tt {

// IB error code: "Unable to connect as the client id is already in use."
inline constexpr int kTwsClientIdInUse = 326;

// How long a client that drew a 326 waits between connect attempts, instead of
// the ordinary 3 s. Long enough that a conflict lasting all afternoon costs four
// attempts a minute of nothing rather than twenty, short enough that the common
// case — the gateway releasing our own just-killed session's id — is recovered
// from well inside the 09:25 auto-start's runway.
inline constexpr int kTwsClientIdRetrySec = 15;

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
