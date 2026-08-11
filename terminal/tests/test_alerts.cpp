// AlertNotifier's webhook payload must be ASCII: ntfy.sh serves any non-ASCII
// request body as a downloadable "attachment.txt" instead of an inline message,
// so an alert containing an em-dash (RISK HALT / KILL SWITCH / PROTECTIVE STOP
// lines all do) would otherwise reach the phone unreadable. detail::ascii_fold
// collapses each run of non-ASCII bytes to a single '-'.
#include "doctest.h"

#include "alert_rules.h"
#include "alerts.h"

#include "engine/tws_client_id.h"

using tt::ui::AlertClass;
using tt::ui::classify_alert;
using tt::ui::detail::ascii_fold;

TEST_CASE("ascii_fold: pure-ASCII text is unchanged") {
    const std::string s = "WATCHDOG broker disconnected for 62s - check IB Gateway (2FA?)";
    CHECK(ascii_fold(s) == s);
}

TEST_CASE("ascii_fold: an em-dash becomes a single hyphen") {
    // "—" is U+2014 = 3 UTF-8 bytes; the whole run collapses to one '-'.
    CHECK(ascii_fold("RISK HALT \xE2\x80\x94 flatten") == "RISK HALT - flatten");
}

TEST_CASE("ascii_fold: adjacent non-ASCII bytes collapse to one hyphen") {
    // en-dash then em-dash back to back -> a single '-', not several.
    CHECK(ascii_fold("a\xE2\x80\x93\xE2\x80\x94""b") == "a-b");
}

TEST_CASE("ascii_fold: result is always pure ASCII") {
    const std::string folded = ascii_fold("caf\xC3\xA9 \xE2\x80\x94 na\xC3\xAFve");
    for (unsigned char c : folded) CHECK(c < 0x80);
}

TEST_CASE("ascii_fold: empty stays empty") {
    CHECK(ascii_fold("").empty());
}

TEST_CASE("classify_alert: critical events page as Critical") {
    CHECK(classify_alert("live: KILL SWITCH — flattening") == AlertClass::Critical);
    CHECK(classify_alert("live: RISK HALT (daily loss limit) — ...") == AlertClass::Critical);
    CHECK(classify_alert("WATCHDOG broker disconnected ...") == AlertClass::Critical);
    CHECK(classify_alert("PROTECTIVE STOP REJECTED on SOXL — naked") == AlertClass::Critical);
}

TEST_CASE("classify_alert: a routine data-feed reconnect does NOT page") {
    // The exact line that flapped all weekend and spammed the phone.
    CHECK(classify_alert(
              "tws-data: history request unanswered for >20s - reconnecting data "
              "session (half-open)") == AlertClass::None);
    CHECK(classify_alert("tws-data: connecting to IB Gateway at 127.0.0.1:4002") ==
          AlertClass::None);
    CHECK(classify_alert("feed error (req 27) tws: connection lost fetching SNXX") ==
          AlertClass::None);
}

TEST_CASE("classify_alert: a genuine half-open ORDER still pages as Warning") {
    CHECK(classify_alert("order #123 unacked for 27s — possible half-open order") ==
          AlertClass::Warning);
    CHECK(classify_alert("live: order #5 rejected by broker") == AlertClass::Warning);
    CHECK(classify_alert("tws-feed: stream lost, reconnecting") == AlertClass::Warning);
}

TEST_CASE("classify_alert: a refused trading day reaches the operator") {
    // Before 0.16.0 the lineup always started SOMETHING, so "no session today"
    // was not a reachable outcome. It is now, and it must not be silent — these
    // lines also bypass route() (which files anything "lineup:" into
    // optimizer.log, which /logs and /events never serve).
    CHECK(classify_alert("lineup: ABORTED - not one of 6 picks produced a usable "
                         "parameter set.") == AlertClass::Critical);
    CHECK(classify_alert("lineup: EXCLUDED MUU, SOXS - no fit from this morning's "
                         "tournament") == AlertClass::Warning);
    // Ordinary lineup prose must stay quiet, or the daily build pages six times.
    CHECK(classify_alert("lineup: ready - 6 symbols loaded into the Trade tabs") ==
          AlertClass::None);
    CHECK(classify_alert("lineup: tournament 3/6 - KORU") == AlertClass::None);
}

TEST_CASE("classify_alert: fills are Info, plain lines are None") {
    CHECK(classify_alert("live: fill #7 BUY 100 @ 12.34") == AlertClass::Info);
    CHECK(classify_alert("candles: SOXL 5m x9750") == AlertClass::None);
    CHECK(classify_alert("tws: reconcile: complete") == AlertClass::None);
}

TEST_CASE("classify_alert: a client-id collision pages, from all three clients") {
    // 2026-08-11: the GUI was started while a headless dry run held the TWS
    // client ids, and IB error 326 repeated every 3 s for seven attempts with no
    // session, no alert and no explanation. The operator reported it as a crash.
    //
    // The line is BUILT here rather than pasted, so the tag classify_alert
    // matches and the tag the adapters emit cannot drift apart: if
    // tws_client_id_conflict_line stops carrying kTwsClientIdConflictTag, this
    // fails instead of the alert silently going quiet again.
    for (const char* who : {"market data", "the ORDER path", "the live tick stream"}) {
        const std::string l =
            tt::tws_client_id_conflict_line(who, "127.0.0.1", 4002, 9);
        CHECK(classify_alert(l) == AlertClass::Critical);
    }
    // ...and it says, in words, what is wrong and what to do about it. IB's own
    // wording ("the client id is already in use") is jargon to whoever is
    // looking at the log at 16:33 on a Tuesday.
    const std::string l = tt::tws_client_id_conflict_line("market data", "127.0.0.1", 4002, 9);
    CHECK(l.find("another program is already connected") != std::string::npos);
    CHECK(l.find("Close it") != std::string::npos);
    CHECK(l.find("Not retrying") != std::string::npos);
    CHECK(l.find("127.0.0.1:4002") != std::string::npos);
}

TEST_CASE("classify_alert: ordinary talk about client ids does NOT page") {
    // The tag is uppercase and specific for the same reason the lineup's
    // verdicts are: a routine line that happens to mention a client id must not
    // wake anyone at 3am. Only the conflict line carries the tag.
    CHECK(classify_alert("tws: connecting to IB Gateway at 127.0.0.1:4002") ==
          AlertClass::None);
    CHECK(classify_alert("tws-data: using client id 9") == AlertClass::None);
    CHECK(classify_alert("tws-feed: client id in use by this session") ==
          AlertClass::None);
}
