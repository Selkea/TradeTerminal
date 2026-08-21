// AlertNotifier's webhook payload must be ASCII: ntfy.sh serves any non-ASCII
// request body as a downloadable "attachment.txt" instead of an inline message,
// so an alert containing an em-dash (RISK HALT / KILL SWITCH / PROTECTIVE STOP
// lines all do) would otherwise reach the phone unreadable. detail::ascii_fold
// collapses each run of non-ASCII bytes to a single '-'.
#include "doctest.h"

#include "alert_rules.h"
#include "alerts.h"

#include "engine/tws_client_id.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using tt::ui::AlertClass;
using tt::ui::classify_alert;
using tt::ui::AlertNotifier;
using tt::ui::detail::ascii_fold;

// A UNIT SUITE MUST NOT MAKE NOISE. notify() calls MessageBeep for every
// admitted Warning and Critical, which is a real system sound on whatever
// desktop the suite runs on — and the flood tests below raise them by the
// dozen. Running the suite was audible; running a mutation sweep, which runs it
// once per mutant, was continuous. Silenced before main, process-wide.
static const bool kSuiteIsSilent = [] {
    AlertNotifier::set_beeps_enabled(false);
    return true;
}();

// The emitters whose TIER depends on their prose live behind a live socket, so
// they are audited as text. Loud, not skipped: an audit that quietly no-ops when
// it cannot find its subject is how the hole stays open.
static std::string read_repo_file(const char* rel) {
    const std::string path = std::string(TT_REPO_DIR) + rel;
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

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
    // The generic "rejected" rule still catches anything that is NOT one of the
    // engine's own tagged refusal lines (an adapter complaint, a gateway line).
    CHECK(classify_alert("tws: order 41 rejected by the gateway") == AlertClass::Warning);
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

TEST_CASE("classify_alert: the refusal tiers, and why they differ") {
    // THE POLICY, pinned (see run_live's note_refusal and engine/reject.h).
    //
    // A single refused ENTRY is Info: webhook, no beep. Most refusals are the
    // system working - the notional clamp finding a position already at its cap,
    // the entry gate declining to buy into a shut exchange. Beeping on each
    // would train the operator to ignore beeps, which is exactly what happened
    // to the "half-open" data-feed line above.
    CHECK(classify_alert("live: ORDER REFUSED [session_closed] KORU buy 234: the "
                         "exchange is not open for new entries right now") ==
          AlertClass::Info);
    // The SAME symbol and cause, three times: no longer a decision, a strategy
    // stuck in a loop it cannot leave. That symbol is silently not trading.
    CHECK(classify_alert("live: ORDER REFUSED REPEATEDLY [notional_cap] KORU buy "
                         "234: the position is already at its dollar cap (x3)") ==
          AlertClass::Warning);
    // An EXIT refused - the position it would have closed is still open and one
    // fewer thing is watching it. Critical on the FIRST occurrence, no repeat
    // threshold, because there is no benign version of it.
    CHECK(classify_alert("live: EXIT ORDER REFUSED [max_order_qty] MUU sell 161: "
                         "the order quantity exceeds the per-order share limit") ==
          AlertClass::Critical);
    // Ordering guard: all three tags nest as substrings, so a classifier that
    // tested the generic one first would downgrade the other two.
    CHECK(std::string(tt::kExitOrderRefusedTag).find(tt::kOrderRefusedTag) !=
          std::string::npos);
    CHECK(std::string(tt::kOrderRefusedRepeatTag).find(tt::kOrderRefusedTag) !=
          std::string::npos);
    // Lowercase "refused" in ordinary prose must stay silent - the lineup and
    // the data feed both use the word.
    CHECK(classify_alert("lineup: refused SOXS, no fit") == AlertClass::None);
}

TEST_CASE("classify_alert: a refusal the BROKER caused is tiered by its tag") {
    // THE TIER BUG THE 0.23.0 SUITE MISSED, because every case above uses a
    // local slug. reject_cause_slug(BrokerRejected) is the literal string
    // "broker_rejected", and IB's own text is "Order rejected - reason:...", so
    // a broker-caused refusal line contains "rejected" twice. Tested BELOW the
    // generic "rejected" rule, every entry IBKR refused on 2026-08-13 beeped
    // Warning while the policy it was written under rates it Info.
    CHECK(classify_alert("live: ORDER REFUSED [broker_rejected] KORU buy 234: Order "
                         "rejected - reason:Exchange is closed.") == AlertClass::Info);
    // ...and the exit stays Critical, for the same reason in reverse.
    CHECK(classify_alert("live: EXIT ORDER REFUSED [broker_rejected] MUU sell 161: "
                         "Order rejected - reason:Exchange is closed.") ==
          AlertClass::Critical);
    CHECK(classify_alert("live: ORDER REFUSED REPEATEDLY [broker_rejected] MUU buy "
                         "161: Order rejected - reason:Exchange is closed. (x3)") ==
          AlertClass::Warning);
    // The engine's trace line for the SAME event, which repeats the broker's
    // text next to the order id. It must not page a second time: one refusal,
    // one tier, decided by the tagged line above.
    CHECK(classify_alert("live: order #7 refused by broker [broker_rejected 201 Order "
                         "rejected - reason:Exchange is closed.]") == AlertClass::None);
}

TEST_CASE("classify_alert: fills are Info, plain lines are None") {
    // THIS CASE IS WHY THE DEAD RULE SURVIVED. It pinned "live: fill #7 ...", the
    // PRE-0.4.3 emitter format, and kept passing for two weeks while production
    // emitted "live: SOXL fill BUY 100 @ 12.34 (order #7)" and every real fill
    // classified as None. A rule and its test can agree with each other and both
    // disagree with the code that actually emits the line — so the string here is
    // now the one engine.cpp really produces (see the 0.29.3 case below).
    CHECK(classify_alert("live: SOXL fill BUY 100 @ 12.34 (order #7)") ==
          AlertClass::Info);
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
            tt::tws_client_id_conflict_line(who, "nothing works", "127.0.0.1", 4002, 9);
        CHECK(classify_alert(l) == AlertClass::Critical);
    }
    // ...and it says, in words, what is wrong and what to do about it. IB's own
    // wording ("the client id is already in use") is jargon to whoever is
    // looking at the log at 16:33 on a Tuesday.
    const std::string l = tt::tws_client_id_conflict_line(
        "market data", "no charts, no warmup, no daily lineup", "127.0.0.1", 4002, 20);
    CHECK(l.find("already connected with it") != std::string::npos);
    CHECK(l.find("no charts, no warmup, no daily lineup") != std::string::npos);
    CHECK(l.find("127.0.0.1:4002") != std::string::npos);
    CHECK(l.find("client id 20") != std::string::npos);
}

TEST_CASE("classify_alert: the conflict line names BOTH causes and promises a retry") {
    // 0.20.0 shipped a sentence that asserted one diagnosis — "It is almost
    // certainly a second TradeTerminal ... Close it (Task Manager:
    // tt_terminal.exe), then restart this app" — and declared "Not retrying:
    // this cannot clear itself". Both are wrong for the 326 this app produces
    // most often: after a hard kill (main.cpp's TerminateProcess teardown
    // budget) or a nightly gateway restart, the id it collides with is its OWN
    // previous session's, held by the gateway for a few seconds more. There is
    // no second tt_terminal.exe to close, and the remedy printed would have the
    // operator kill the running trading app.
    const std::string l = tt::tws_client_id_conflict_line("the ORDER path", "no orders",
                                                          "127.0.0.1", 4002, 20);
    CHECK(l.find("second TradeTerminal") != std::string::npos);       // cause 1
    CHECK(l.find("own previous session") != std::string::npos);       // cause 2
    CHECK(l.find("Retrying every") != std::string::npos);
    // The claims the app can no longer make. "Not retrying" would be a lie the
    // moment io_loop's backoff runs, and telling an operator to restart the app
    // is what turns a self-clearing blip into a manual outage.
    CHECK(l.find("Not retrying") == std::string::npos);
    CHECK(l.find("restart this app") == std::string::npos);
}

TEST_CASE("classify_alert: the all-clear is NOT a Critical page") {
    // A conflict that can end has to be observably ended, or the operator is
    // left acting on a page that stopped being true. But good news must not
    // arrive as a Critical: the cleared tag is deliberately not a substring of
    // the conflict tag, and this is what pins that apart.
    const std::string ok = tt::tws_client_id_cleared_line("the ORDER path", 20);
    CHECK(ok.find(tt::kTwsClientIdConflictTag) == std::string::npos);
    CHECK(classify_alert(ok) != AlertClass::Critical);
    CHECK(ok.find("no action needed") != std::string::npos);
    CHECK(ok.find("client id 20") != std::string::npos);
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

// --------------------------------------------- the 326 state machine, audited
//
// The 0.20.0 branch latched IB error 326 and never cleared it. `exchange(true)`
// appeared in all three clients and `store(false)` appeared nowhere, so the
// first collision a client ever saw ended it for the life of the process: a
// live session with no order path while positions were open, a data client with
// no candles for the rest of the day, and — because Watch-IbGateway.ps1 keys its
// cold relaunch off broker_connected, which could then never come back — IB
// Gateway restarted every four minutes until someone noticed.
//
// Exercising the real state machine needs a second API client on a live gateway,
// which no test here has. So the invariant is audited as text: a client that can
// LATCH the condition must also be able to CLEAR it, and its I/O loop must still
// reach a retry. It is a weaker test than driving the sockets and it is the one
// available; without it, deleting the clear is green.
TEST_CASE("every TWS client that latches a 326 can also clear it and retry") {
    auto count = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + 1))
            ++n;
        return n;
    };
    for (const char* rel : {"/engine/src/tws_broker.cpp", "/engine/src/tws_feed.cpp",
                            "/terminal/src/net/tws_data.cpp"}) {
        const std::string path = std::string(TT_REPO_DIR) + rel;
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << path);
        const std::string src{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>()};
        INFO("in " << rel);
        // Set once, on the refusal...
        CHECK(count(src, "client_id_conflict_.exchange(true") == 1);
        // ...and cleared once, on the handshake that proves the id is ours. THIS
        // is the assertion whose absence was the 0.20.0 defect.
        CHECK(count(src, "client_id_conflict_.exchange(false") == 1);
        // Nothing may pin it true by another route.
        CHECK(count(src, "client_id_conflict_.store(true") == 0);
        // And the I/O loop must still get to a connect attempt while it is set —
        // slowed to the 326 cadence, not parked. A client that only ever slept
        // here would satisfy the two counts above and still never recover.
        //
        // Either spelling counts. The ORDERS client moved to the tiered helper
        // in 0.22.0 (fast retries first, then the slow cadence) because its
        // client id is now fixed at kTwsOrdersClientId: with no other id to move
        // to, the retry cadence IS the recovery, and 15 s on every lineup swap
        // would be a 15 s order-path outage with positions live at the broker.
        // The FEED client still rotates (App hands out 40-59) and uses the flat
        // constant. The DATA client does NOT rotate — tws_data.h pins it to 9 —
        // and this comment claimed for three versions that it did. That was not
        // idle: the fixed-id premise is the whole reason the 60 s page window
        // exists, so a comment asserting the opposite is part of why the data
        // path went on paging instantly until 0.34.2.
        CHECK(count(src, "kTwsClientIdRetrySec") + count(src, "tws_client_id_retry_sec") >= 1);

        // 0.34.2: ALL THREE now hold the page for kTwsClientIdPageAfterSec.
        //
        // The window was written for a fixed id, where the overwhelmingly common
        // 326 is our own just-reaped socket clearing in seconds — and only the
        // ORDER client honoured it. The other two called the TAGGED builder
        // straight from EWrapper::error, so the same gateway, at the same
        // instant, for the same cause, paged Critical from two clients and
        // stayed silent from the third.
        INFO("the delayed page must live in the loop that knows the elapsed time");
        CHECK(count(src, "kTwsClientIdPageAfterSec") >= 1);
        // The immediate line is the UNTAGGED one...
        CHECK(count(src, "tws_client_id_waiting_line") == 1);
        // ...and the tagged builder is called exactly once, from the io_loop.
        CHECK(count(src, "tws_client_id_conflict_line") == 1);
        // The tagged ALL-CLEAR is gated too: it pages Warning, and closing an
        // outage nobody was told about is the same noise from the other side.
        CHECK(count(src, "if (conflict_paged)") == 1);
    }
}

TEST_CASE("the data client's two 60s levers are distinct and both still armed") {
    // tws_data.cpp carries TWO sixty-second constants for the same episode, and
    // they do different things: kConflictSettleMs errors queued candle requests
    // back to their callers, kTwsClientIdPageAfterSec releases the paging tag.
    // 0.34.2 put them on ONE clock (io.conflict_since_ms) because the file
    // having the timestamp for only the first is exactly how the second went
    // missing for three versions — the clock and the number were both already
    // here, and it still paged instantly.
    const std::string src = read_repo_file("/terminal/src/net/tws_data.cpp");
    CHECK(src.find("steady_ms() - io.conflict_since_ms > kConflictSettleMs") !=
          std::string::npos);
    CHECK(src.find("steady_ms() - io.conflict_since_ms >= kTwsClientIdPageAfterSec * 1000") !=
          std::string::npos);
    // Neither may collapse into "the latch is set, act now": settling on sight
    // would fail a lineup's fetches for a blip a plain reconnect rides out.
    CHECK(src.find("kConflictSettleMs") != std::string::npos);
    CHECK(src.find("conflict_since = ") == std::string::npos);   // the old loop local
}

TEST_CASE("the first thing a refused client says never pages") {
    // The tier contract the window rests on, exercised for real rather than
    // audited: the immediate sentence explains itself and stays silent, and only
    // the line io_loop emits after kTwsClientIdPageAfterSec pages Critical.
    for (const char* who : {"market data", "the live tick stream", "the ORDER path"}) {
        const std::string quiet =
            tt::tws_client_id_waiting_line(who, 9, tt::kTwsClientIdRetrySec);
        INFO("who=" << who);
        CHECK(tt::ui::classify_alert(quiet) == tt::ui::AlertClass::None);
        // Silent is not the same as absent — 2026-08-11's defect was SILENCE,
        // so the sentence must still name the id and say it is retrying.
        CHECK(quiet.find("client id 9") != std::string::npos);
        CHECK(quiet.find("retrying") != std::string::npos);
        // And it must promise the page, or the operator cannot tell a withheld
        // alert from a missing one.
        CHECK(quiet.find(std::to_string(tt::kTwsClientIdPageAfterSec)) !=
              std::string::npos);
        // It must NOT carry the paging tag. This is the whole mechanism: the
        // difference between the two lines is the tag, not the words.
        CHECK(quiet.find(tt::kTwsClientIdConflictTag) == std::string::npos);
    }
    // ...and the delayed one still pages, from every client.
    const std::string loud = tt::tws_client_id_conflict_line(
        "market data", "no charts, no warmup, no daily lineup", "127.0.0.1", 4002, 9);
    CHECK(tt::ui::classify_alert(loud) == tt::ui::AlertClass::Critical);
    CHECK(tt::ui::classify_alert_category(loud) == tt::ui::AlertCategory::Connection);
}

// ---------------------------------------------------------------------------
// A STRATEGY'S OWN LOG LINE, and the 73 pages in 90 seconds of 2026-08-17.
//
// EngineCtx::log stamps the strategy's chosen level into the prefix. Tiering on
// that beats keyword-matching prose written by six different strategy authors,
// which is what these lines used to get.

TEST_CASE("classify_alert: a strategy's refused entry is Info, not a page") {
    // THE EXACT LINE that sent 73 pages in 90 seconds during a lineup build
    // (donchian_trend.cpp: ctx.log(2, "entry rejected")). It contains
    // "rejected", so it fell through to the generic rule and paged Warning —
    // walking around the Info tier the engine's own refusal tag sits in.
    CHECK(classify_alert("[strategy warn] entry rejected") == AlertClass::Info);
    CHECK(classify_alert("[strategy warn] SOXS: entry rejected") == AlertClass::Info);
    // Same tier for the other level-2 lines: working-as-designed or self-healing.
    CHECK(classify_alert("[strategy warn] breakout orders died before triggering "
                         "— re-arming") == AlertClass::Info);
    CHECK(classify_alert("[strategy warn] time_stop was 0 (no risk control) — "
                         "restored to 12 bars") == AlertClass::Info);
}

TEST_CASE("classify_alert: a strategy reporting a naked position IS Critical") {
    // The bug in the other direction, and the more dangerous of the two. Every
    // strategy spells this in LOWERCASE; the Critical tag at the top of the
    // rules is the engine's UPPERCASE spelling and never matched it. So a
    // strategy saying its protective stop was refused and the position is
    // unprotected paged at exactly the same Warning as one declining to buy.
    CHECK(classify_alert("[strategy error] protective stop rejected — position "
                         "unprotected") == AlertClass::Critical);
    CHECK(classify_alert("[strategy error] SOXL: protective stop rejected — "
                         "flattening") == AlertClass::Critical);
    CHECK(classify_alert("[strategy error] EOD flatten died — position still "
                         "open") == AlertClass::Critical);
}

TEST_CASE("classify_alert: the engine's own tags still outrank strategy prose") {
    // The strategy rules are checked BELOW every engine tag, so they can only
    // ever tier a line the engine did not classify itself. If this inverts, a
    // strategy's wording could silently downgrade a refusal the engine rated.
    CHECK(classify_alert("PROTECTIVE STOP REJECTED on SOXL — naked") ==
          AlertClass::Critical);
    CHECK(classify_alert("[strategy warn] live: KILL SWITCH — flattening") ==
          AlertClass::Critical);
}

TEST_CASE("alerts: a SIMULATED event never reaches the alert scan at all") {
    // Source-text pin, and the only tool available: nothing in the suite
    // constructs an App, so App::tick's drain loop is unreachable from a test.
    //
    // The optimizer's backtests run on the SAME Engine object as the live
    // session, so its log ring carries both. alert_scan used to run above the
    // origin branch and paged for both. The pin asserts the scan is INSIDE the
    // from_live arm — the rules above would all still pass with it hoisted back
    // out, and the phone would ring for a strategy that only exists in a sweep.
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    const size_t drain = src.find("while (engine_.pop_log(line, from_live))");
    REQUIRE(drain != std::string::npos);
    const size_t guard = src.find("if (from_live) {", drain);
    const size_t scan = src.find("alert_scan(line);", drain);
    REQUIRE(guard != std::string::npos);
    REQUIRE(scan != std::string::npos);
    CHECK(guard < scan);   // the origin is decided BEFORE anything can page
}

// ---------------------------------------------------------------------------
// DELIVERY IS NOT ASSUMED.
//
// The webhook worker used to call curl_easy_perform and discard the result, so
// a 429, a 500 and a DNS failure were all indistinguishable from delivery. On
// 2026-08-17 the day's two CRITICAL pages — an off-lineup broker position and a
// confirmed book divergence — were emitted, logged, and never arrived, and
// nothing in the app recorded it. These pin the accounting that makes that
// state visible.

TEST_CASE("alerts: a notifier with no webhook counts nothing and never blocks") {
    AlertNotifier n;
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    const auto d = n.delivery();
    CHECK(d.sent == 0);
    CHECK(d.failed == 0);
    CHECK(d.dropped == 0);   // never queued: no webhook is configured, not a loss
}

TEST_CASE("alerts: a muted notifier queues nothing") {
    AlertNotifier n;
    n.set_muted(true);
    n.notify(AlertNotifier::Critical, "RISK HALT");
    CHECK(n.delivery().sent == 0);
    CHECK(n.delivery().dropped == 0);
}

TEST_CASE("alerts: a MUTED notifier records what it threw away") {
    // The Alerts menu item can turn the whole channel off, and until 0.31.2 a
    // muted notifier discarded everything — Criticals included — leaving no
    // trace anywhere that it had. A muted app and a quiet one produced byte-for-
    // byte identical diagnostics, which is the confusion 0.29.2 exists to end,
    // reachable from a menu.
    AlertNotifier n;
    n.set_webhook("x-no-such-scheme://drop");
    n.set_muted(true);
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    n.notify(AlertNotifier::Critical, "PROTECTIVE STOP REJECTED on SOXL");
    n.notify(AlertNotifier::Info, "live: SOXL fill BUY 100 @ 12.34 (order #7)");
    const auto d = n.delivery();
    CHECK(d.muted_discarded == 3);
    CHECK(d.sent == 0);
    // Counted apart from every other loss: a backlog overflow, a rate cap and a
    // muted channel are three different problems with three different fixes.
    CHECK(d.dropped == 0);
    CHECK(d.throttled == 0);
    CHECK(d.coalesced == 0);
}

TEST_CASE("alerts: unmuting resumes delivery, and the discard count stands") {
    // The count is a record of what was missed, so it must not be reset by
    // turning the channel back on.
    AlertNotifier n;
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    n.set_muted(true);
    n.notify(AlertNotifier::Critical, "RISK HALT while muted");
    REQUIRE(n.delivery().muted_discarded == 1);
    n.set_muted(false);
    n.notify(AlertNotifier::Critical, "RISK HALT after unmuting");
    const auto d = n.delivery();
    CHECK(d.muted_discarded == 1);   // still says one page was lost
    CHECK(d.coalesced == 0);         // and the second one really was admitted
}

TEST_CASE("alerts: an unreachable webhook is recorded as FAILED, not as sent") {
    // The whole point: a page that did not arrive must leave a trace. Port 9 is
    // the discard service and nothing listens on it, so this fails at the
    // transport with no network round trip to anywhere real.
    AlertNotifier n;
    n.set_retry_backoff_ms(20);
    n.set_webhook("x-no-such-scheme://drop");
    n.notify(AlertNotifier::Warning, "unreachable-on-purpose");
    // Warning retries twice with a 3s then 6s backoff; wait past that.
    AlertNotifier::Delivery d{};
    for (int i = 0; i < 300; ++i) {
        d = n.delivery();
        if (d.failed > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(d.failed == 1);
    CHECK(d.sent == 0);
    CHECK(d.retried == 2);            // it did not give up on the first refusal
    CHECK_FALSE(d.last_error.empty());   // and it says why
}

// A one-shot HTTP server that answers with `status` and nothing else. Needed
// because the failure that lost the 2026-08-17 Critical pages was a RATE LIMIT
// — HTTP 429 — which is a perfectly successful transport carrying a refusal.
// Testing against a closed port only exercises curl's transport error and
// leaves the status check unproven: with the status check deleted, a
// closed-port case still passes. This one does not.
namespace {
struct OneShotHttp {
    SOCKET listener = INVALID_SOCKET;
    std::thread th;
    unsigned short port = 0;
    mutable std::mutex m;
    std::vector<std::string> reqs;   // what actually went out on the wire

    // The bodies received, so a test can assert what the operator would read
    // rather than only that a counter moved.
    std::vector<std::string> bodies() const {
        std::lock_guard lk(m);
        std::vector<std::string> out;
        for (const std::string& r : reqs) {
            const size_t h = r.find("\x0d\x0a\x0d\x0a");
            out.push_back(h == std::string::npos ? r : r.substr(h + 4));
        }
        return out;
    }

    explicit OneShotHttp(std::vector<int> statuses) {
        WSADATA w{};
        WSAStartup(MAKEWORD(2, 2), &w);
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener != INVALID_SOCKET);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;   // ephemeral
        REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        int alen = sizeof a;
        REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&a), &alen) == 0);
        port = ntohs(a.sin_port);
        REQUIRE(listen(listener, 8) == 0);
        th = std::thread([this, statuses] {
            for (int status : statuses) {
                SOCKET c = accept(listener, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                char buf[2048];
                const int got = recv(c, buf, sizeof buf, 0);   // drain the request
                if (got > 0) {
                    std::lock_guard lk(m);
                    reqs.emplace_back(buf, static_cast<size_t>(got));
                }
                const std::string crlf = "\x0d\x0a";
                const std::string resp = "HTTP/1.1 " + std::to_string(status) +
                                         " X" + crlf + "Content-Length: 0" + crlf +
                                         "Connection: close" + crlf + crlf;
                send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
                closesocket(c);
            }
        });
    }
    ~OneShotHttp() {
        closesocket(listener);
        if (th.joinable()) th.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/topic";
    }
};

AlertNotifier::Delivery wait_settled(const AlertNotifier& n, int tries = 400) {
    AlertNotifier::Delivery d{};
    for (int i = 0; i < tries; ++i) {
        d = n.delivery();
        if (d.sent > 0 || d.failed > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return d;
}

// ---------------------------------------------------------------------------
// 0.30.0 SEAMS. Two things changed underneath the queue tests below.
//
// First, "flood 1".."flood 400" is now ONE burst key: burst_key folds numbers
// precisely so a repeating event with a rolling order id collapses instead of
// looking like 400 new events. That is the flood defence doing its job, and it
// means a test can no longer reach the QUEUE by repeating itself.
//
// Second, distinct lines are rate-capped per severity, so 400 unique Infos do
// not reach it either.
//
// So the queue tests vary their text non-numerically AND drive a fake clock
// forward far enough to keep a token available, which leaves the backlog cap as
// the only policy still in the way — the thing they were written to test.

struct TestClock {
    std::shared_ptr<std::atomic<int64_t>> t =
        std::make_shared<std::atomic<int64_t>>(1);
    void advance(int64_t ms) { t->fetch_add(ms); }
    std::function<int64_t()> fn() const {
        auto p = t;
        return [p] { return p->load(); };
    }
};

// Letters, not digits: a numeric suffix folds to a single burst key.
std::string distinct_line(int i) {
    std::string s = "flood ";
    for (int k = 0, v = i; k < 3; ++k, v /= 26) s.push_back(char('a' + v % 26));
    return s;
}

// Info refills at kRatePerMin[Info] = 6/min, i.e. one token per 10 s.
constexpr int64_t kInfoTokenMs = 10000;

void saturate_backlog(AlertNotifier& n, TestClock& clk, int count = 400) {
    for (int i = 0; i < count; ++i) {
        clk.advance(kInfoTokenMs);
        n.notify(AlertNotifier::Info, distinct_line(i));
    }
}
} // namespace

TEST_CASE("alerts: HTTP 429 is a FAILURE, not a delivery") {
    // THE EXACT FAILURE of 2026-08-17. ntfy.sh answers a depleted rate-limit
    // bucket with 429; the transport succeeds, so the old code — which only
    // ever looked at curl's return value — counted it as sent. The Critical
    // book-divergence page went out into that and nothing recorded its loss.
    OneShotHttp srv({429, 429, 429});   // all three attempts get 429
    AlertNotifier n;
    n.set_retry_backoff_ms(20);
    n.set_webhook(srv.url());
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    const auto d = wait_settled(n);
    CHECK(d.sent == 0);
    CHECK(d.failed == 1);
    CHECK(d.last_status == 429);
    CHECK(d.last_error == "HTTP 429");
}

TEST_CASE("alerts: a 200 is a delivery, and is not retried") {
    // The positive control. Without it, a rule that called EVERYTHING a failure
    // would satisfy the case above just as well.
    OneShotHttp srv({200});
    AlertNotifier n;
    n.set_retry_backoff_ms(20);
    n.set_webhook(srv.url());
    n.notify(AlertNotifier::Critical, "RISK HALT");
    const auto d = wait_settled(n);
    CHECK(d.sent == 1);
    CHECK(d.failed == 0);
    CHECK(d.retried == 0);
    CHECK(d.last_status == 200);
    CHECK(d.last_error.empty());
}

TEST_CASE("alerts: a Critical page RECOVERS when the bucket refills") {
    // WHY THE RETRY EXISTS. ntfy replenishes a depleted bucket over seconds, so
    // the page refused at T is very likely accepted a moment later — and that
    // is exactly the window a single best-effort shot threw away on
    // 2026-08-17. First attempt 429, second 200: the page gets through, and the
    // operator hears about the divergence instead of never knowing.
    OneShotHttp srv({429, 200});
    AlertNotifier n;
    n.set_retry_backoff_ms(20);
    n.set_webhook(srv.url());
    n.notify(AlertNotifier::Critical, "PROTECTIVE STOP REJECTED");
    const auto d = wait_settled(n);
    CHECK(d.sent == 1);        // delivered on the retry
    CHECK(d.failed == 0);
    CHECK(d.retried == 1);
    CHECK(d.last_status == 200);
}

TEST_CASE("alerts: the delivery counters survive a backlog overflow") {
    // The queue caps at 100 so a dead webhook cannot hoard forever. What is
    // dropped there was never sent and must not read as success.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_retry_backoff_ms(20);
    n.set_worker_paused(true);   // the ADMISSION accounting is the unit, not the drain
    n.set_webhook("x-no-such-scheme://drop");
    saturate_backlog(n, clk);
    const auto d = n.delivery();
    CHECK(d.dropped > 0);
    CHECK(d.sent == 0);
}

// ---------------------------------------------------------------------------
// FINDINGS FROM THE 2026-08-19 ALERT-PATH AUDIT (0.29.3).

TEST_CASE("classify_alert: the EOD backstop is the last resort and pages Critical") {
    // It was AlertClass::None for its entire life. Reaching this line means every
    // strategy-level exit failed and the engine had to cancel-all and force-flatten
    // a position it should never still have been holding.
    CHECK(classify_alert("live: EOD BACKSTOP \xE2\x80\x94 open position(s) past 15:57 with no "
                         "strategy closing them; broker cancel-all + flatten requested") ==
          AlertClass::Critical);
    CHECK(classify_alert("live: EOD BACKSTOP \xE2\x80\x94 open position(s) past 12:57 with no "
                         "strategy closing them; broker cancel-all + flatten requested") ==
          AlertClass::Critical);
}

TEST_CASE("classify_alert: a live fill is Info, in the format actually emitted") {
    // THE DEAD RULE. It tested has("live: fill"), but engine.cpp emits
    // "live: %s fill %s %.0f @ %.2f (order #%llu)" — the SYMBOL sits between
    // "live: " and "fill", so the substring never occurred and every fill since
    // 0.4.3 (2026-08-05) classified as None.
    CHECK(classify_alert("live: SOXL fill BUY 100 @ 12.34 (order #7)") == AlertClass::Info);
    CHECK(classify_alert("live: SPCH fill SELL 533 @ 9.56 (order #41)") == AlertClass::Info);
    // The literal the old rule looked for must not be what makes it pass now.
    CHECK(classify_alert("live: fill") == AlertClass::None);
    // ...and ordinary prose containing the word must stay silent.
    CHECK(classify_alert("optimizer: 312 backtests, no fill model change") ==
          AlertClass::None);
}

TEST_CASE("alerts: a Critical is not discarded because Infos filled the queue") {
    // The in-process twin of the 2026-08-17 loss. The cap consulted only size, so
    // a Critical arriving into a full queue was dropped while the stale Infos
    // ahead of it were kept.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_retry_backoff_ms(20);
    n.set_worker_paused(true);   // hold the drain: the ADMISSION policy is the unit
    n.set_webhook("x-no-such-scheme://drop");
    saturate_backlog(n, clk);
    const auto before = n.delivery();
    REQUIRE(before.dropped > 0);        // the queue really is saturated
    // A Critical now evicts the oldest Info rather than being refused. It is
    // accepted, so the only observable is that `dropped` rises by exactly one
    // (the evicted Info) and never by two (evicted + refused).
    const auto a = n.delivery();
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    const auto b = n.delivery();
    // ACCEPTED by evicting an Info, not refused. The two are separate counters
    // precisely because they are otherwise indistinguishable: a dropped-only
    // accounting rises by exactly one either way, so the test could not tell
    // the fix from its absence (it did not, until this was split out).
    CHECK(b.evicted == a.evicted + 1);
    CHECK(b.dropped == a.dropped);
}

TEST_CASE("alerts: an Info arriving into a full queue is still simply dropped") {
    // The other half, so the eviction rule cannot degenerate into "always evict".
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_retry_backoff_ms(20);
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    saturate_backlog(n, clk);
    const uint64_t d0 = n.delivery().dropped;
    const uint64_t e0 = n.delivery().evicted;
    clk.advance(kInfoTokenMs);   // hand it a token, so it is the QUEUE that refuses
    n.notify(AlertNotifier::Info, "one more line free of digits");
    CHECK(n.delivery().dropped == d0 + 1);   // refused at the door
    CHECK(n.delivery().evicted == e0);       // and no peer was sacrificed for it
}

// ---------------------------------------------------------------------------
// 0.30.0: BURST COALESCING AND THE PER-SEVERITY RATE CAP.
//
// Everything above this line bounds what happens to an alert AFTER something
// decides to raise one. None of it bounds how MANY get raised, which is what
// actually went wrong on 2026-08-17 — and 0.29.1 fixed that one emitter, by
// hand, leaving every other repeat-emitter in the codebase able to do it again.
// The audit found four more. This layer stops the class instead of the
// instances.

TEST_CASE("classify_alert: the raw IB error trace is a trace, not a third page") {
    // 2026-08-13: all six lineup symbols signalled into a closed exchange. IB's
    // text is "Order rejected - reason:Exchange is closed.", which contains
    // "rejected", so the adapter's raw trace fell through to the generic keyword
    // rule and paged WARNING — a third copy of an event the policy rates Info,
    // at a LOUDER tier than the tagged line that decides the policy, and with no
    // throttle of any kind.
    CHECK(classify_alert("tws: IB ERROR 201 (id 5): Order rejected - reason:Exchange "
                         "is closed.") == AlertClass::None);
    CHECK(classify_alert("tws: IB ERROR 202 (id 7): Order Canceled - reason:") ==
          AlertClass::None);
    // THE LINES THAT SETTLED THE TIER, taken verbatim from the VPS log for one
    // ordinary overnight IBC restart: six 1100s inside 42 seconds and a pair of
    // 502s, all routine, all silent before this tag existed. Info still posts to
    // the webhook, so tiering the trace there would have added phone traffic to
    // a nightly maintenance window while claiming to fix a flood.
    CHECK(classify_alert("tws-data: IB ERROR 1100 (id -1): Connectivity between IBKR "
                         "and Trader Workstation has been lost.") == AlertClass::None);
    CHECK(classify_alert("tws-data: IB ERROR 502 (id -1): Couldn't connect to TWS. "
                         "Confirm that \"Enable ActiveX and Socket Clients\" is "
                         "enabled") == AlertClass::None);
    // What actually reports a gateway that does NOT come back: state, not the
    // callback, and only once it has lasted.
    CHECK(classify_alert("alert: WATCHDOG gateway lost its connection to IBKR (error "
                         "1100) for 62s during a live session - check IB Gateway") ==
          AlertClass::Critical);
}

TEST_CASE("classify_alert: a Critical tag inside a trace still outranks it") {
    // The trace rule sits below every Critical tag on purpose, so it can only
    // ever demote lines that nothing louder has claimed.
    //
    // These lines carry BOTH a Critical tag and the trace tag, which is the only
    // shape that can tell the ordering apart. The obvious test — a plain
    // DUPLICATE ORDER ID line — asserts nothing about it: that line contains no
    // "IB ERROR" at all, so it passes just as happily with the trace rule moved
    // to the top of the function. Its mutant SURVIVED until these existed.
    const std::string dup = std::string("tws: ") + tt::kIbErrorTraceTag + " 103 (id 5): " +
                            tt::kTwsDuplicateOrderIdTag + " on this account";
    CHECK(classify_alert(dup) == AlertClass::Critical);
    const std::string naked = std::string("tws: ") + tt::kIbErrorTraceTag +
                              " 201 (id 7): PROTECTIVE STOP REJECTED";
    CHECK(classify_alert(naked) == AlertClass::Critical);
    // ...while the same shape with nothing louder in it stays a trace.
    CHECK(classify_alert(std::string("tws: ") + tt::kIbErrorTraceTag +
                         " 201 (id 7): Order rejected - reason:") == AlertClass::None);
}

TEST_CASE("the adapters do not word a pre-send refusal 'order rejected'") {
    // SOURCE-TEXT PIN, because no test constructs a broker: TwsBroker::submit
    // needs an EClientSocket and a live gateway to reach.
    //
    // The classifier test above proves what the CLASSIFIER does with the new
    // wording. It cannot prove the adapter still uses it — a mutant that reverted
    // the log string sailed through the whole suite, because the test feeds
    // classify_alert a literal and never asks what the emitter emits. The tier
    // here is decided by a keyword in prose, so the prose is the contract.
    const std::string tws = read_repo_file("/engine/src/tws_broker.cpp");
    const std::string ibkr = read_repo_file("/engine/src/ibkr_broker.cpp");
    CHECK(tws.find("log(\"order rejected") == std::string::npos);
    CHECK(ibkr.find("log(\"order rejected") == std::string::npos);
    CHECK(tws.find("log(\"order refused before send:") != std::string::npos);
    CHECK(ibkr.find("log(\"order refused before send:") != std::string::npos);
    // And the raw IB error trace goes out under the tag, not bare "error N".
    CHECK(tws.find("b.log(std::string(kIbErrorTraceTag)") != std::string::npos);
}

TEST_CASE("the feeds pace a dropped session instead of retrying on a fixed 1s") {
    // SOURCE-TEXT PIN, same reason: the reconnect loops need a live websocket.
    // The defect was written out three times and could be reintroduced in any
    // one of them, so all three are audited — a fix that holds in two feeds and
    // regresses in the third is the shape this project keeps finding.
    for (const char* rel : {"/engine/src/finnhub_feed.cpp",
                            "/engine/src/polygon_feed.cpp",
                            "/engine/src/ibkr_feed.cpp"}) {
        const std::string src = read_repo_file(rel);
        CHECK_MESSAGE(src.find("on_session_lost(net_steady_ms())") != std::string::npos, rel);
        // The exact expression that produced 60 pages a minute.
        CHECK_MESSAGE(src.find("next_connect_ms = net_steady_ms() + 1000;") ==
                      std::string::npos, rel);
        // ...and no feed resets the pacing merely because a handshake returned.
        CHECK_MESSAGE(src.find("backoff_s = 1;") == std::string::npos, rel);
    }
}

TEST_CASE("classify_alert: a refusal the adapter caught before sending does not page twice") {
    // These logged "order rejected: ..." and so tripped the generic rule at
    // Warning, once per SUBMIT and outside note_refusal's repeat throttle — a
    // gateway that was down while six strategies kept signalling paged on every
    // bar. The tagged ORDER REFUSED line is the one that carries the tier.
    CHECK(classify_alert("tws: order refused before send: the TWS socket API is not "
                         "connected") == AlertClass::None);
    CHECK(classify_alert("ibkr: order refused before send: the gateway session is not "
                         "ready") == AlertClass::None);
    // ...and the tagged line still says it, at the tier the policy chose.
    CHECK(classify_alert(std::string("live: ") + tt::kOrderRefusedTag +
                         " SOXL broker_not_connected") == AlertClass::Info);
}

TEST_CASE("classify_alert: the feed reconnect line still pages, and still coalesces") {
    // Retiering was NOT the fix for the feed flood — the pacing was (see
    // engine/feed_reconnect.h). This line must keep its Warning: a feed that
    // genuinely cannot hold a session is worth hearing about once.
    CHECK(classify_alert("finnhub: stream lost, reconnecting in 1s") ==
          AlertClass::Warning);
    CHECK(classify_alert("finnhub: stream lost, reconnecting in 30s") ==
          AlertClass::Warning);
    // And because burst_key folds the number, the climbing backoff does not turn
    // one repeating event into thirty distinct ones.
    CHECK(tt::ui::detail::burst_key("finnhub: stream lost, reconnecting in 1s") ==
          tt::ui::detail::burst_key("finnhub: stream lost, reconnecting in 30s"));
}

// ---------------------------------------------------------------------------
// SETTINGS > NOTIFICATIONS: per-category muting.

using tt::ui::AlertCategory;
using tt::ui::classify_alert_category;

TEST_CASE("alert categories: each kind of event lands where the menu says") {
    // The axis the operator actually wants to silence along. classify_alert says
    // how URGENT; this says ABOUT WHAT, and they must not disagree about which
    // tag wins — both check Risk-tier tags first.
    CHECK(classify_alert_category("live: SOXL fill BUY 100 @ 12.34 (order #7)") ==
          AlertCategory::Trades);
    CHECK(classify_alert_category("live: ORDER REFUSED [session_closed] KORU buy 234") ==
          AlertCategory::Orders);
    CHECK(classify_alert_category("live: RISK HALT (daily loss limit)") ==
          AlertCategory::Risk);
    CHECK(classify_alert_category("live: EOD BACKSTOP - open position(s) past 15:57") ==
          AlertCategory::Risk);
    CHECK(classify_alert_category("PROTECTIVE STOP REJECTED on SOXL") ==
          AlertCategory::Risk);
    CHECK(classify_alert_category("WATCHDOG broker disconnected for 62s") ==
          AlertCategory::Connection);
    CHECK(classify_alert_category("finnhub: stream lost, reconnecting in 4s") ==
          AlertCategory::Connection);
    CHECK(classify_alert_category("alert: BOOK DIVERGENCE NVDA app=0 broker=20") ==
          AlertCategory::Integrity);
    CHECK(classify_alert_category("tws: reconcile: OFF-LINEUP BROKER POSITION NVDA 20") ==
          AlertCategory::Integrity);
    CHECK(classify_alert_category("lineup: EXCLUDED MSTZ, KORU, RAM - no fit") ==
          AlertCategory::Lineup);
    // Anything unrecognised is System, never silently folded into Trades — a
    // new alert must not arrive pre-muted because someone had fills switched off.
    CHECK(classify_alert_category("something nobody has categorised yet") ==
          AlertCategory::System);
}

TEST_CASE("alert categories: a refused EXIT is Risk, not Orders") {
    // THE ONE THAT MATTERS. "Orders" is the category an operator switches off to
    // stop hearing about routine refusals — and a refused exit leaves a position
    // open with one fewer thing watching it. classify_alert already rates it
    // Critical; it must not be silenceable by the Orders switch.
    const std::string exit_refused =
        "live: EXIT ORDER REFUSED [max_order_qty] MUU sell 161: the order "
        "quantity exceeds the per-order share limit";
    CHECK(classify_alert(exit_refused) == AlertClass::Critical);
    CHECK(classify_alert_category(exit_refused) == AlertCategory::Risk);
    CHECK(tt::ui::alert_category_is_safety(AlertCategory::Risk));
    CHECK(tt::ui::alert_category_is_safety(AlertCategory::Integrity));
    CHECK_FALSE(tt::ui::alert_category_is_safety(AlertCategory::Trades));
}

TEST_CASE("alerts: a disabled category is discarded, and the loss is counted") {
    AlertNotifier n;
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    n.set_categorizer([](const std::string& l) {
        return static_cast<int>(classify_alert_category(l));
    });
    n.set_category_enabled(static_cast<int>(AlertCategory::Trades), false);
    n.notify(AlertNotifier::Info, "live: SOXL fill BUY 100 @ 12.34 (order #7)");
    n.notify(AlertNotifier::Info, "live: SNDQ fill SELL 343 @ 15.35 (order #2)");
    const auto d = n.delivery();
    CHECK(d.category_discarded[static_cast<int>(AlertCategory::Trades)] == 2);
    // Counted apart from every other loss: a muted channel, a full backlog, a
    // rate cap and a switched-off category are four different problems.
    CHECK(d.muted_discarded == 0);
    CHECK(d.dropped == 0);
    CHECK(d.throttled == 0);
    CHECK(d.coalesced == 0);
}

TEST_CASE("alerts: switching one category off leaves the others armed") {
    // The whole point. Silencing fills must not silence a naked position.
    AlertNotifier n;
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    n.set_categorizer([](const std::string& l) {
        return static_cast<int>(classify_alert_category(l));
    });
    n.set_category_enabled(static_cast<int>(AlertCategory::Trades), false);
    n.notify(AlertNotifier::Info, "live: SOXL fill BUY 100 @ 12.34 (order #7)");
    n.notify(AlertNotifier::Critical, "PROTECTIVE STOP REJECTED on SOXL");
    n.notify(AlertNotifier::Critical, "alert: BOOK DIVERGENCE NVDA app=0 broker=20");
    const auto d = n.delivery();
    CHECK(d.category_discarded[static_cast<int>(AlertCategory::Trades)] == 1);
    CHECK(d.category_discarded[static_cast<int>(AlertCategory::Risk)] == 0);
    CHECK(d.category_discarded[static_cast<int>(AlertCategory::Integrity)] == 0);
}

TEST_CASE("alerts: with no categoriser set, nothing is ever gated") {
    // The pre-0.32.0 behaviour, kept reachable so a notifier constructed without
    // App wiring (every other test in this file) cannot silently drop alerts.
    AlertNotifier n;
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    n.set_category_enabled(0, false);   // would gate everything, if it applied
    n.notify(AlertNotifier::Critical, "live: RISK HALT (daily loss limit)");
    for (int i = 0; i < AlertNotifier::kMaxCategories; ++i)
        CHECK(n.delivery().category_discarded[i] == 0);
}

TEST_CASE("alerts: the gate runs BEFORE coalescing, so a muted burst costs nothing") {
    // Order matters: a switched-off alert must not open a burst window that a
    // later WANTED alert of the same shape would then be folded into, nor spend
    // a rate-cap token.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    n.set_categorizer([](const std::string& l) {
        return static_cast<int>(classify_alert_category(l));
    });
    n.set_category_enabled(static_cast<int>(AlertCategory::Trades), false);
    for (int i = 0; i < 40; ++i)
        n.notify(AlertNotifier::Info, "live: SOXL fill BUY 100 @ 12.34 (order #7)");
    const auto d = n.delivery();
    CHECK(d.category_discarded[static_cast<int>(AlertCategory::Trades)] == 40);
    CHECK(d.coalesced == 0);    // no burst was ever opened
    CHECK(d.throttled == 0);    // and no token was spent
    CHECK(d.summaries == 0);    // nothing to summarise: it was never in flight
}

using tt::ui::detail::burst_key;

TEST_CASE("burst_key: a rolling id does not make one repeating event look like many") {
    // The reason exact-text matching would have been useless here: the same
    // event almost never repeats byte-for-byte. It carries an order id, a share
    // count, a price.
    CHECK(burst_key("error 201 (id 5): Order rejected - reason:Exchange is closed.") ==
          burst_key("error 201 (id 6): Order rejected - reason:Exchange is closed."));
    CHECK(burst_key("live: SPCH fill BUY 533 @ 9.38") ==
          burst_key("live: SPCH fill BUY 486 @ 10.27"));
}

TEST_CASE("burst_key: a sentence-ending period is not swallowed as part of a number") {
    // If '.' folded unconditionally, "closed." and "closed" would be different
    // keys — and IB's own text ends in one, so the burst would never coalesce.
    CHECK(burst_key("Exchange is closed.") == "Exchange is closed.");
    CHECK(burst_key("saw 12.5 pct") == "saw # pct");
    CHECK(burst_key("qty 1,250 filled") == "qty # filled");
    // The cases that actually pin the rule down. Above, the '.' is nowhere near
    // a digit, so a build that folded separators unconditionally would pass all
    // three — and one did: this test's own mutant SURVIVED until these two lines
    // existed. The separator has to be reached from inside a number run for the
    // distinction to be observable at all.
    CHECK(burst_key("closed 20. next") == "closed #. next");
    CHECK(burst_key("a 5, b 6") == "a #, b #");
}

TEST_CASE("burst_key: genuinely different prose stays different") {
    // Folding numbers must not fold everything: two unrelated failures have to
    // stay two keys or one would silence the other.
    CHECK(burst_key("stream lost, reconnecting") !=
          burst_key("gateway lost its connection to IBKR"));
    CHECK(burst_key("BOOK DIVERGENCE NVDA app=0 broker=20") !=
          burst_key("OFF-LINEUP BROKER POSITION NVDA 20 @ 209.17"));
}

TEST_CASE("alerts: 2026-08-17 replayed - one line repeated 73 times pages ONCE") {
    // THE INCIDENT, at the layer that now stops it. donchian_trend emitted the
    // same refusal from inside 73 sweep cells and every copy paged the phone.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 73; ++i) {
        clk.advance(100);   // 7.3 s: the whole burst inside one window
        n.notify(AlertNotifier::Info, "[strategy warn] SOXL entry rejected");
    }
    const auto d = n.delivery();
    CHECK(d.coalesced == 72);   // one page; seventy-two folded into it
    CHECK(d.throttled == 0);    // and the rate cap was never even reached
}

TEST_CASE("alerts: at the incident's real pace the window turns over, and no more") {
    // 73 pages in 90 s against a 60 s window: the burst spans one boundary, so
    // the honest outcome is two pages and a summary — not one, and emphatically
    // not seventy-three.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 73; ++i) {
        clk.advance(90000 / 73);
        n.notify(AlertNotifier::Info, "[strategy warn] SOXL entry rejected");
    }
    const auto d = n.delivery();
    const uint64_t paged = 73 - d.coalesced - d.throttled;
    CHECK(paged <= 2);
    CHECK(d.summaries >= 1);   // and the operator is told what was swallowed
}

TEST_CASE("alerts: the swallowed copies are REPORTED when the window closes") {
    // Suppression that leaves no trace is the 0.29.2 bug rebuilt one layer up.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 50; ++i)
        n.notify(AlertNotifier::Warning, "stream lost, reconnecting");
    CHECK(n.delivery().coalesced == 49);
    CHECK(n.delivery().summaries == 0);   // the window is still open
    clk.advance(61000);
    n.notify(AlertNotifier::Info, "an unrelated line");   // drives the flush
    CHECK(n.delivery().summaries == 1);
}

TEST_CASE("alerts: a flood that STOPS still gets its summary, with nothing to trigger it") {
    // The case a lazy flush would miss, and the one that matters most: the
    // burst ends, nothing else is ever raised, and the operator is left holding
    // the single page that opened it with no idea 49 more followed. The worker
    // has to close the window on its own clock.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_flush_interval_ms(10);
    n.set_retry_backoff_ms(5);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 50; ++i)
        n.notify(AlertNotifier::Warning, "stream lost, reconnecting");
    clk.advance(61000);
    // Deliberately no further notify().
    for (int i = 0; i < 400 && n.delivery().summaries == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(n.delivery().summaries == 1);
}

TEST_CASE("alerts: the first page is immediate and the summary carries the count") {
    // End to end, on a real socket: what the operator actually reads. The first
    // copy must never be delayed or batched — suppression applies only to the
    // second and later copies of something already sent.
    OneShotHttp srv({200, 200});
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_flush_interval_ms(10);
    n.set_retry_backoff_ms(5);
    n.set_webhook(srv.url());
    for (int i = 0; i < 50; ++i)
        n.notify(AlertNotifier::Warning, "stream lost, reconnecting");
    clk.advance(61000);
    for (int i = 0; i < 400 && n.delivery().sent < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto b = srv.bodies();
    REQUIRE(b.size() == 2);
    CHECK(b[0] == "stream lost, reconnecting");   // sent on the first copy, unaltered
    CHECK(b[1].find("[+49 more like this in the last 61s]") != std::string::npos);
}

TEST_CASE("alerts: a burst summary keeps the severity of the burst it closes") {
    // Observable through the retry rule: Warning/Critical retry, Info does not.
    // A summary silently demoted to Info would become the tier nobody is woken
    // for, which is how this defence would turn back into the silence it
    // replaced.
    OneShotHttp srv({200, 500, 500, 500});
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_flush_interval_ms(10);
    n.set_retry_backoff_ms(5);
    n.set_webhook(srv.url());
    for (int i = 0; i < 4; ++i)
        n.notify(AlertNotifier::Critical, "PROTECTIVE STOP REJECTED on SOXL");
    clk.advance(61000);
    for (int i = 0; i < 400 && n.delivery().failed == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto d = n.delivery();
    CHECK(d.summaries == 1);
    CHECK(d.retried == 2);   // 3 attempts on the summary: it is still Critical
    CHECK(d.failed == 1);
}

TEST_CASE("alerts: a flood of DISTINCT alerts is capped, and the cap is counted") {
    // Coalescing cannot help when every line differs — the per-severity bucket
    // is the backstop for that, and reaching it must never be silent.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 40; ++i)   // no clock advance: no refill
        n.notify(AlertNotifier::Info, distinct_line(i));
    const auto d = n.delivery();
    CHECK(d.coalesced == 0);                 // every line really was its own key
    CHECK(d.throttled == 40 - 6);            // Info's bucket holds six
}

TEST_CASE("alerts: an Info flood cannot spend the budget a Critical needs") {
    // The starvation the per-severity buckets exist to prevent, and the same
    // property the queue's severity-aware eviction gives one layer down. A
    // single shared cap would have let the 73 junk pages lock out the two
    // Criticals that followed them.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 200; ++i)
        n.notify(AlertNotifier::Info, distinct_line(i));
    const uint64_t t0 = n.delivery().throttled;
    REQUIRE(t0 > 0);   // Info's bucket really is empty
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    CHECK(n.delivery().throttled == t0);   // the Critical went straight out
}

TEST_CASE("alerts: the rate cap refills, so a capped channel recovers on its own") {
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 40; ++i)
        n.notify(AlertNotifier::Info, distinct_line(i));
    const uint64_t t0 = n.delivery().throttled;
    REQUIRE(t0 > 0);
    clk.advance(kInfoTokenMs);   // exactly one Info token
    n.notify(AlertNotifier::Info, distinct_line(900));
    CHECK(n.delivery().throttled == t0);   // admitted, not refused
}

TEST_CASE("alerts: rate-capped alerts are summarised ONCE, not once each") {
    // WHAT MUTATION TESTING FOUND, and the reason this counter is per-severity.
    // The first cut recorded every throttled alert as its own burst, so 194
    // refused Infos became 194 summary pages the instant the window closed —
    // and summaries bypass the rate cap deliberately, so the defence would have
    // emitted a bigger flood than the one it exists to stop. Nothing caught it
    // because nothing yet asserted what a throttled alert TURNS INTO.
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_worker_paused(true);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 200; ++i)
        n.notify(AlertNotifier::Info, distinct_line(i));
    REQUIRE(n.delivery().throttled > 100);
    clk.advance(61000);
    n.notify(AlertNotifier::Warning, "an unrelated line to drive the flush");
    CHECK(n.delivery().summaries == 1);   // one report, not one per swallowed line
}

TEST_CASE("alerts: the rate-cap report says how many it swallowed, on the wire") {
    // Info's bucket holds six, so twenty distinct lines page six times and the
    // other fourteen are refused — then reported, once, in terms the operator
    // can act on.
    OneShotHttp srv({200, 200, 200, 200, 200, 200, 200});
    AlertNotifier n;
    TestClock clk;
    n.set_clock_for_test(clk.fn());
    n.set_flush_interval_ms(10);
    n.set_retry_backoff_ms(5);
    n.set_webhook(srv.url());
    for (int i = 0; i < 20; ++i)
        n.notify(AlertNotifier::Info, distinct_line(i));
    clk.advance(61000);
    for (int i = 0; i < 400 && n.delivery().sent < 7; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto b = srv.bodies();
    REQUIRE(b.size() == 7);   // six pages plus exactly one report
    CHECK(b.back().find("ALERT RATE CAP: 14 further alerts suppressed") !=
          std::string::npos);
    CHECK(b.back().find("most recent:") != std::string::npos);
}

TEST_CASE("alerts: a single alert is never coalesced, throttled or delayed") {
    // The overwhelmingly common case, and the one this layer must not touch.
    OneShotHttp srv({200});
    AlertNotifier n;
    n.set_retry_backoff_ms(20);
    n.set_webhook(srv.url());
    n.notify(AlertNotifier::Critical, "BOOK DIVERGENCE NVDA app=0 broker=20");
    const auto d = wait_settled(n);
    CHECK(d.sent == 1);
    CHECK(d.coalesced == 0);
    CHECK(d.throttled == 0);
    CHECK(d.summaries == 0);
}

// ---------------------------------------------------------------------------
// 0.34.1: UNREACHABLE TIME STOP — severity has to track HAZARD, not the tag.

TEST_CASE("an unreachable time stop that is still LIVE keeps paging Critical") {
    // Unchanged, and the reason the tag exists: 2026-08-14, STKH live on a
    // 233-bar time stop against a 77-bar day, no price stop, -$506 in 2h16m.
    const std::string kept =
        std::string("live: ") + tt::ui::kUnreachableStopTag +
        " on STKH - KEPT in the session because the broker book is not confirmed "
        "flat there";
    CHECK(tt::ui::classify_alert(kept) == tt::ui::AlertClass::Critical);
    CHECK(tt::ui::classify_alert_category(kept) == tt::ui::AlertCategory::Risk);
    CHECK(tt::ui::alert_category_is_safety(
        tt::ui::classify_alert_category(kept)));

    // ...and "no symbol survived, there is no trading day" stays Critical too.
    const std::string none = std::string("live: not starting - every symbol has an ") +
                             tt::ui::kUnreachableStopTag + ". Refit them.";
    CHECK(tt::ui::classify_alert(none) == tt::ui::AlertClass::Critical);
    CHECK(tt::ui::classify_alert_category(none) == tt::ui::AlertCategory::Risk);
}

TEST_CASE("a guard that SUCCEEDED does not page Critical") {
    // THE DEFECT. classify_alert matched the tag alone, with no regard for the
    // verb, so "we refused the unrealizable fit and kept a good one" arrived at
    // the same tier, in the same safety category, as "we are live on one". The
    // autopilot re-runs every 30 minutes, so that is a Critical channel being
    // trained out of the operator — the 2026-08-17 failure mode exactly.
    const std::string declined =
        std::string("autopilot: STKH - ") + tt::ui::kUnreachableStopDeclinedTag +
        " (233 bars, only 77 fit in a trading day at this bar size). Keeping the "
        "incumbent; the fit is unrealizable, not merely worse.";
    CHECK(tt::ui::classify_alert(declined) == tt::ui::AlertClass::None);

    // Session start dropped them: real news, once a day, nothing exposed.
    const std::string refused =
        std::string("live: ") + tt::ui::kUnreachableStopRefusedTag +
        " on STKH, SNDQ: longer than a trading day. Starting without them.";
    CHECK(tt::ui::classify_alert(refused) == tt::ui::AlertClass::Warning);

    // NEITHER is Risk. Risk is a safety category and therefore unsilenceable;
    // filing "the app declined a bad fit" there is what made the tier useless.
    // Both are news about what trades today, which is what Lineup means — and
    // it is where the lineup's own EXCLUDED verdict for this same fact lands.
    for (const std::string& l : {declined, refused}) {
        CHECK(tt::ui::classify_alert_category(l) == tt::ui::AlertCategory::Lineup);
        CHECK_FALSE(tt::ui::alert_category_is_safety(
            tt::ui::classify_alert_category(l)));
    }
}

TEST_CASE("the quiet variants nest inside the loud tag, and order decides") {
    // Both CONTAIN kUnreachableStopTag on purpose: the operator keeps seeing the
    // phrase they already know and only the tier changes. That makes both
    // classifiers order-dependent — test the trap, not just the result.
    const std::string d = tt::ui::kUnreachableStopDeclinedTag;
    const std::string r = tt::ui::kUnreachableStopRefusedTag;
    REQUIRE(d.find(tt::ui::kUnreachableStopTag) != std::string::npos);
    REQUIRE(r.find(tt::ui::kUnreachableStopTag) != std::string::npos);
    // ...so the specific cases must be answered BEFORE the general one.
    const std::string src = read_repo_file("/terminal/src/alert_rules.h");
    const size_t decl = src.find("has(kUnreachableStopDeclinedTag)) return AlertClass::None");
    const size_t refu = src.find("has(kUnreachableStopRefusedTag)) return AlertClass::Warning");
    const size_t base = src.find("has(kUnreachableStopTag)) return AlertClass::Critical");
    REQUIRE(decl != std::string::npos);
    REQUIRE(refu != std::string::npos);
    REQUIRE(base != std::string::npos);
    CHECK(decl < base);
    CHECK(refu < base);
    // Same ordering trap in the CATEGORY function, where getting it wrong is
    // worse: Risk is a safety category, so the line becomes unsilenceable.
    // Scoped to the function body: "KILL SWITCH" also appears in classify_alert
    // and in prose above it, and an unscoped find lands on the first of those.
    const size_t fn = src.find("inline AlertCategory classify_alert_category(");
    REQUIRE(fn != std::string::npos);
    const size_t cat_quiet = src.find(
        "has(kUnreachableStopDeclinedTag) || has(kUnreachableStopRefusedTag)", fn);
    const size_t cat_risk = src.find("has(\"KILL SWITCH\")", fn);
    REQUIRE(cat_quiet != std::string::npos);
    REQUIRE(cat_risk != std::string::npos);
    CHECK(cat_quiet < cat_risk);
}
