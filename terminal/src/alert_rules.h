#pragma once
// Push-alert classification for a log line, kept as a pure function separate
// from AlertNotifier's beep/webhook side effects so the rules are unit-testable
// and there's ONE place that decides what pages the phone.

#include "engine/tws_client_id.h"   // kTwsClientIdConflictTag
#include "net/book_divergence.h"    // kBookDivergenceTag

#include <string>

namespace tt::ui {

enum class AlertClass { None, Info, Warning, Critical };

inline AlertClass classify_alert(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    if (has("KILL SWITCH") || has("RISK HALT") || has("WATCHDOG") ||
        has("PROTECTIVE STOP REJECTED"))
        return AlertClass::Critical;
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
    if (has("rejected") || has("stream lost") || has("auth failed") ||
        has("(drops!)") || has("half-open order"))
        return AlertClass::Warning;
    if (has("live: fill"))
        return AlertClass::Info;
    return AlertClass::None;
}

} // namespace tt::ui
