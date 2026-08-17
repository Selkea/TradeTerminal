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

#include <chrono>
#include <thread>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using tt::ui::AlertClass;
using tt::ui::classify_alert;
using tt::ui::AlertNotifier;
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
        // The feed and data clients still rotate and still use the constant.
        CHECK(count(src, "kTwsClientIdRetrySec") + count(src, "tws_client_id_retry_sec") >= 1);
    }
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
                recv(c, buf, sizeof buf, 0);   // drain the request
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
    n.set_retry_backoff_ms(20);
    n.set_webhook("x-no-such-scheme://drop");
    for (int i = 0; i < 400; ++i)
        n.notify(AlertNotifier::Info, "flood " + std::to_string(i));
    const auto d = n.delivery();
    CHECK(d.dropped > 0);
    CHECK(d.sent == 0);
}
