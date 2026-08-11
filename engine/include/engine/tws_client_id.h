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
// and the incident was reported as "it crashed". Two separate defects:
//
//  1. IB's own wording is the only explanation offered, and it describes an API
//     concept ("client id") the person looking at the log has no reason to know.
//     What they need to be told is that something else is already talking to
//     this gateway and only one thing may.
//  2. It was RETRIED. 326 is not a transient error: it is refused because
//     another live socket owns that id, and that stays true for as long as the
//     other process lives. Reconnecting on the 3 s loop can never succeed; it
//     only buries the one line that mattered under a repeating three-line
//     stanza.
//
// The three TWS clients (data id 9, feed id 8, orders id 7) each connect
// independently and each can hit this on its own, so the constant and the
// sentence live here rather than being written out three times and drifting.

#include <string>

namespace tt {

// IB error code: "Unable to connect as the client id is already in use."
inline constexpr int kTwsClientIdInUse = 326;

// The token every conflict line carries, uppercase and fixed, so
// tt::ui::classify_alert can page on it without pattern-matching prose that a
// later edit would silently break. Uppercase for the same reason the lineup's
// verdicts are: ordinary log prose about clients or ids must not page anyone.
inline constexpr const char* kTwsClientIdConflictTag = "API CLIENT ID IN USE";

// The operator-facing line. `who` names which of our three clients was refused
// in plain words ("market data", "market data stream", "orders"), NOT the class
// name or the id — the id is IB's vocabulary and appears once, in parentheses,
// for whoever goes digging.
//
// The remedy is stated because it is not guessable from the error: the fix is
// never "retry", it is "find the other program and close it". A second
// TradeTerminal is the overwhelmingly likely culprit (the dry-run script, a
// minimized window, a leftover window-less process holding the mutex), so it is
// named first.
inline std::string tws_client_id_conflict_line(const char* who,
                                               const std::string& host, int port,
                                               int client_id) {
    return std::string(kTwsClientIdConflictTag) + ": another program is already "
           "connected to IB Gateway at " + host + ":" + std::to_string(port) +
           " and IB allows only one at a time, so " + who + " is OFF. It is "
           "almost certainly a second TradeTerminal - a dry run, a minimized "
           "window, or a leftover tt_terminal.exe with no window. Close it "
           "(Task Manager: tt_terminal.exe), then restart this app. Not "
           "retrying: this cannot clear itself while the other program runs "
           "(IB error " + std::to_string(kTwsClientIdInUse) + ", client id " +
           std::to_string(client_id) + ").";
}

} // namespace tt
