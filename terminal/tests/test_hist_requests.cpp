// The reqId lifecycle of a retried historical-data request.
//
// This is the 2026-08-10 regression, in full: retry_dead_history cancelled a
// dead request and re-issued it on the SAME reqId, so IB's cancel
// acknowledgement (error 162, one processMsgs later) matched the FRESHLY
// RE-ISSUED request and erased it — 520 retries, 512 acks, same second, all
// session. Every downstream symptom followed from that one erase: bars returned
// for the retry were dropped as "unknown reqId", the died-twice escalation
// became unreachable dead code (the data-session reconnect it triggers had
// recovered this exact failure 286 times before the retry feature landed), and
// /diag's pending_history / oldest_history_age_ms read 0 through a 184-minute
// outage.
//
// tws_data.cpp's Io struct cannot be constructed without a live EClientSocket,
// so the id policy was lifted into net/hist_requests.h to be testable at all.
#include "doctest.h"

#include "net/hist_requests.h"

#include <string>

using tt::net::HistPending;
using tt::net::HistRequests;
using tt::net::kCancelAckGraceMs;
using tt::net::kHistTimeoutMs;

namespace {

constexpr int64_t kT0 = 1'000'000;   // arbitrary steady-clock origin (ms)

// Queue one request exactly as Io::pump_requests does.
void add(HistRequests& h, int req_id, const std::string& sym, int64_t now) {
    HistPending p;
    p.pub_id = static_cast<uint32_t>(req_id);
    p.symbol = sym;
    p.interval = "5m";
    p.dur = "6 M";
    p.bar = "5 mins";
    p.sent_ms = p.first_sent_ms = now;
    h.add(req_id, std::move(p));
}

// What Io::error() does with an incoming error, in ITS dispatch order: the
// awaiting-ack set is offered the id BEFORE the live set, which is the whole
// fix. Returned so a test can tell "absorbed my own cancel" apart from "killed
// a live request".
enum class ErrOutcome { Absorbed, ErasedLive, Ignored };
ErrOutcome deliver_error(HistRequests& h, int id) {
    if (h.take_cancel_ack(id)) return ErrOutcome::Absorbed;
    if (h.find(id)) {
        h.erase(id);
        return ErrOutcome::ErasedLive;
    }
    return ErrOutcome::Ignored;
}

// Drive one request to its retry and hand back the new id, as io_loop does.
int retry_once(HistRequests& h, int old_id, int new_id, int64_t now) {
    const auto dead = h.dead_ids(now);
    REQUIRE(dead.size() == 1);
    CHECK(dead[0] == old_id);
    HistPending* p = h.begin_retry(old_id, new_id, now);
    REQUIRE(p != nullptr);
    p->sent_ms = now;   // send_history stamps this on the way out
    return new_id;
}

} // namespace

TEST_CASE("hist ids: a retry is re-issued under a NEW id and retires the old one") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    const int64_t dead_at = kT0 + kHistTimeoutMs + 1;
    retry_once(h, 88, 89, dead_at);

    CHECK(h.find(88) == nullptr);          // the dead id is out of the live set
    REQUIRE(h.find(89) != nullptr);
    CHECK(h.find(89)->symbol == "MUU");
    CHECK(h.find(89)->retried);
    // The caller never hears about the swap: it is still waiting on req 88.
    CHECK(h.find(89)->pub_id == 88u);
    CHECK(h.awaiting_ack() == 1);
}

TEST_CASE("hist ids: the cancel ack kills the OLD request, never the retry") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    retry_once(h, 88, 89, kT0 + kHistTimeoutMs + 1);

    // THE REGRESSION. "API historical data query cancelled: 88" arrives on the
    // next processMsgs; before 0.15.0 id 88 WAS the retry and this erased it.
    CHECK(deliver_error(h, 88) == ErrOutcome::Absorbed);
    REQUIRE(h.find(89) != nullptr);
    CHECK(h.pending() == 1);
    // Exactly one ack is owed, and it is consumed: no second bite, no leak.
    CHECK(h.awaiting_ack() == 0);
    CHECK(deliver_error(h, 88) == ErrOutcome::Ignored);
    CHECK(h.pending() == 1);

    // A genuine error on the retry's own id still reaches the caller.
    CHECK(deliver_error(h, 89) == ErrOutcome::ErasedLive);
    CHECK(h.pending() == 0);
}

TEST_CASE("hist ids: the retry's bars are accepted, the dead id's are dropped") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    retry_once(h, 88, 89, kT0 + kHistTimeoutMs + 1);

    // historicalData/historicalDataEnd both key off find(): a late bar for the
    // cancelled request has no home, so it cannot be mistaken for the answer.
    CHECK(h.find(88) == nullptr);
    HistPending* live = h.find(89);
    REQUIRE(live != nullptr);
    CHECK(live->candles.empty());   // begin_retry drops anything IB sent for 88
    live->candles.push_back({0, 1, 1, 1, 1, 1});
    CHECK(h.find(89)->candles.size() == 1);
}

TEST_CASE("hist ids: the died-twice escalation is reachable again") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    const int64_t retried_at = kT0 + kHistTimeoutMs + 1;
    retry_once(h, 88, 89, retried_at);

    // The retry gets a FULL fresh window - it must not escalate on the age of
    // the attempt it replaced.
    CHECK_FALSE(h.has_twice_dead(retried_at));
    CHECK_FALSE(h.has_twice_dead(retried_at + kHistTimeoutMs));
    // ...and it must not be retried a second time either.
    CHECK(h.dead_ids(retried_at + kHistTimeoutMs + 1).empty());
    // Dead again: this is what drives io_loop's data-session reconnect, the
    // recovery that fired 286 times before the retry feature buried it.
    CHECK(h.has_twice_dead(retried_at + kHistTimeoutMs + 1));

    // The ack for the first cancel does not disarm it (it never touches 89).
    CHECK(deliver_error(h, 88) == ErrOutcome::Absorbed);
    CHECK(h.has_twice_dead(retried_at + kHistTimeoutMs + 1));
}

TEST_CASE("hist ids: /diag sees the outage instead of a pinned zero") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    const int64_t retried_at = kT0 + kHistTimeoutMs + 1;
    retry_once(h, 88, 89, retried_at);
    deliver_error(h, 88);   // the ack that used to empty the pending set

    const int64_t later = retried_at + kHistTimeoutMs;
    // One fetch is outstanding: the retry. The cancelled id is NOT pending —
    // counting it too would report the same fetch twice.
    CHECK(h.pending() == 1);
    // Aged from the FIRST attempt, so the figure is the length of the outage
    // rather than the age of the current attempt (which the retry just reset).
    CHECK(h.oldest_age_ms(later) == static_cast<int>(later - kT0));
    CHECK(h.oldest_age_ms(later) > static_cast<int>(kHistTimeoutMs));
}

TEST_CASE("hist ids: healthy siblings on the same session are untouched") {
    HistRequests h;
    add(h, 10, "SOXS", kT0);                        // dies
    add(h, 11, "AAOX", kT0 + kHistTimeoutMs);       // sent later, still fine
    const int64_t now = kT0 + kHistTimeoutMs + 1;
    const auto dead = h.dead_ids(now);
    REQUIRE(dead.size() == 1);
    CHECK(dead[0] == 10);
    h.begin_retry(10, 12, now);
    CHECK(h.pending() == 2);
    REQUIRE(h.find(11) != nullptr);
    CHECK(h.find(11)->symbol == "AAOX");
    CHECK_FALSE(h.find(11)->retried);
}

TEST_CASE("hist ids: an ack that never arrives is pruned, not leaked") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    const int64_t retried_at = kT0 + kHistTimeoutMs + 1;
    retry_once(h, 88, 89, retried_at);
    CHECK(h.awaiting_ack() == 1);
    h.prune_cancel_acks(retried_at + kCancelAckGraceMs);   // still in grace
    CHECK(h.awaiting_ack() == 1);
    h.prune_cancel_acks(retried_at + kCancelAckGraceMs + 1);
    CHECK(h.awaiting_ack() == 0);
    // At ~520 retries a session this is what keeps the set bounded.
    CHECK(h.pending() == 1);
}

TEST_CASE("hist ids: a dropped session takes the awaiting-ack set with it") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    retry_once(h, 88, 89, kT0 + kHistTimeoutMs + 1);
    h.clear();   // drop_connection
    CHECK(h.pending() == 0);
    // The socket that owed us the ack is gone; holding the entry would leak it
    // across every reconnect.
    CHECK(h.awaiting_ack() == 0);
    CHECK(deliver_error(h, 88) == ErrOutcome::Ignored);
}

TEST_CASE("hist ids: a request inside its window is left alone") {
    HistRequests h;
    add(h, 88, "MUU", kT0);
    CHECK(h.dead_ids(kT0 + kHistTimeoutMs).empty());   // boundary: not yet dead
    CHECK_FALSE(h.has_twice_dead(kT0 + kHistTimeoutMs));
    CHECK(h.pending() == 1);
    CHECK(h.oldest_age_ms(kT0 + 5'000) == 5'000);
}
