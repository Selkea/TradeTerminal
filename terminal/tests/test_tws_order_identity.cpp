// The TWS orders client id, and the order-id high-water mark.
//
// 2026-08-13. App::start_live_session drew a NEW TWS orders client id on every
// session start: `tc.client_id = 20 + (tws_client_seq_++ % 20)`. IB scopes an
// order's orderStatus / execDetails / commissionReport to the client id that
// PLACED it, while reqAllOpenOrders lets ANY client SEE every resting order in
// the account. So at 09:37:21 client 20 placed RAM's bracket, at 09:49:36 an
// automatic lineup swap rebuilt the session on client 22, client 22 adopted both
// resting exits, and when the take-profit filled the callbacks went to a client
// that no longer existed. The app carried a PHANTOM 411-share position for
// 4 h 03 m and reported a fictional +$528 unrealized.
//
// The same rotation caused a second, independent failure: a fresh client's
// nextValidId seed sat BELOW the account-wide high-water mark (59 while the
// placing client had reached ~76), so every subsequent placement collided with
// "error 103: Duplicate order id" — which pushed no reject, so the engine left
// those orders Working forever and nothing said the order path was paralysed.
//
// These cases pin both fixes as arithmetic, plus the source-text invariant that
// the rotation has not crept back in.
#include "doctest.h"

#include "alert_rules.h"
#include "engine/tws_client_id.h"

#include <fstream>
#include <iterator>
#include <string>

using namespace tt;

// ---- the id itself ----------------------------------------------------------

TEST_CASE("orders client id is a constant, and it is 20") {
    // 20 and not any other number: tws_client_seq_ was a member initialised to
    // 0, so the FIRST session of every process placed on 20. That makes 20 the
    // id holding the account's existing resting orders across the deploy —
    // renumbering would strand exactly the orders this change protects, once.
    CHECK(kTwsOrdersClientId == 20);
}

TEST_CASE("the feed id still rotates, disjoint from orders and data") {
    // Rotation is safe on the feed for the reason it was never safe on the order
    // path: the feed places no orders, so nothing is scoped to its id and it has
    // nothing to adopt. It keeps the 326 escape hatch where that hatch is free.
    for (int seq = 0; seq < 100; ++seq) {
        const int id = tws_feed_client_id(seq);
        CHECK(id >= kTwsFeedClientIdBase);
        CHECK(id < kTwsFeedClientIdBase + kTwsFeedClientIdSpan);
        CHECK(id != kTwsOrdersClientId);
        CHECK(id != 9);   // the data client
    }
    CHECK(tws_feed_client_id(0) != tws_feed_client_id(1));   // it really rotates
}

// ---- the retry tier ---------------------------------------------------------

TEST_CASE("326 retries fast first, then backs off") {
    // With a fixed id there is no other id to move to, so the retry cadence IS
    // the recovery. The common collision is now this app's own just-reaped
    // session — seconds — and paying 15 s for it on every lineup swap would mean
    // a 15 s order-path outage with positions live at the broker.
    CHECK(tws_client_id_retry_sec(0) == kTwsClientIdFastRetrySec);
    CHECK(tws_client_id_retry_sec(kTwsClientIdFastRetries - 1) == kTwsClientIdFastRetrySec);
    // ...then the slow cadence the original 15 s was reasoned about: a conflict
    // lasting all afternoon costing four attempts a minute of nothing.
    CHECK(tws_client_id_retry_sec(kTwsClientIdFastRetries) == kTwsClientIdRetrySec);
    CHECK(tws_client_id_retry_sec(9999) == kTwsClientIdRetrySec);
    CHECK(kTwsClientIdFastRetrySec < kTwsClientIdRetrySec);
}

TEST_CASE("the fast tier finishes well inside the page delay") {
    // Otherwise the page would fire while the cheap retries were still running,
    // which is the noise the delay exists to remove.
    CHECK(kTwsClientIdFastRetries * kTwsClientIdFastRetrySec < kTwsClientIdPageAfterSec);
}

// ---- MECHANISM B: the order-id high-water mark ------------------------------

TEST_CASE("order ids only ever move FORWARD") {
    CHECK(tws_advance_order_id(-1, 59) == 59);    // the first seed of a session
    CHECK(tws_advance_order_id(59, 76) == 76);    // adoption raises it
    CHECK(tws_advance_order_id(76, 59) == 76);    // a lower seed is DISCARDED
    CHECK(tws_advance_order_id(76, 76) == 76);    // idempotent
}

TEST_CASE("replaying 2026-08-13: adoption lifts the seed above the collision") {
    // The incident, as arithmetic. Client 22 handshakes and is handed 59, then
    // adopts the client-20 orders resting at 60..75. Before 0.22.0 next_tws_id
    // stayed 59 and the very next placement drew "error 103: Duplicate order
    // id"; every one after it did too, which is why the operator's manual sell
    // was refused for four hours.
    long next = -1;
    next = tws_advance_order_id(next, 59);              // nextValidId
    CHECK(next == 59);
    for (long adopted = 60; adopted <= 75; ++adopted)   // reqAllOpenOrders
        next = tws_advance_order_id(next, adopted + 1);
    CHECK(next == 76);                                  // clear of every live id
    // ...and a reqIds answer that arrives with the server's stale 59 cannot pull
    // it back down.
    CHECK(tws_advance_order_id(next, 59) == 76);
}

TEST_CASE("a mid-session reconnect cannot walk the counter backwards") {
    // The latent second instance of the same bug: nextValidId is per-connection,
    // so a reconnect's seed can be below ids this session has already spent.
    long next = 100;
    CHECK(tws_advance_order_id(next, 42) == 100);
}

// ---- the operator-facing lines ----------------------------------------------

TEST_CASE("a foreign adopted order pages Critical and says why") {
    const std::string line =
        tws_foreign_order_line("RAM", "LMT", 39, 20, kTwsOrdersClientId + 2);
    CHECK(tt::ui::classify_alert(line) == tt::ui::AlertClass::Critical);
    CHECK(line.find("RAM") != std::string::npos);
    CHECK(line.find("client 20") != std::string::npos);
    // It must say the two things the operator cannot infer: we will not hear the
    // fill, and we cannot cancel it.
    CHECK(line.find("NOT be told") != std::string::npos);
    CHECK(line.find("CANNOT cancel") != std::string::npos);
}

TEST_CASE("a duplicate order id pages Critical and reports the self-heal") {
    const std::string line = tws_duplicate_order_id_line(59, 76, kTwsOrdersClientId);
    CHECK(tt::ui::classify_alert(line) == tt::ui::AlertClass::Critical);
    CHECK(line.find("NEVER REACHED THE MARKET") != std::string::npos);
    CHECK(line.find("76") != std::string::npos);   // where the counter went
    CHECK(kTwsDuplicateOrderId == 103);
}

TEST_CASE("a brief client-id collision explains itself WITHOUT paging") {
    // The 2026-08-11 lesson was that the defect was SILENCE, not the retry — so
    // the sentence is emitted immediately. The paging TAG is what waits, because
    // under a fixed id a 326 across a restart is our own socket and pages on
    // every lineup swap would train the operator to swipe them away.
    const std::string quiet = tws_client_id_waiting_line("the ORDER path", 20,
                                                         kTwsClientIdFastRetrySec);
    CHECK(tt::ui::classify_alert(quiet) == tt::ui::AlertClass::None);
    CHECK(quiet.find("client id 20") != std::string::npos);
    CHECK(quiet.find("retrying") != std::string::npos);
    // ...and the one that fires after kTwsClientIdPageAfterSec still pages.
    const std::string loud =
        tws_client_id_conflict_line("the ORDER path", "nothing can reach the market",
                                    "127.0.0.1", 4002, 20);
    CHECK(tt::ui::classify_alert(loud) == tt::ui::AlertClass::Critical);
}

// ---- source-text invariants -------------------------------------------------
//
// The call sites need an EClientSocket and an IB Gateway to execute, so they are
// audited as text. That is weaker than running them, and it is the strongest
// test available here — the alternative is a defect class this project has
// already shipped once, with nothing standing in the way of shipping it again.

static std::string read_repo_file(const char* rel) {
    const std::string path = std::string(TT_REPO_DIR) + rel;
    std::ifstream in(path, std::ios::binary);
    // Loud, not skipped: an audit that quietly no-ops when it cannot find its
    // subject is how the hole stays open.
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

TEST_CASE("app.cpp does not rotate the ORDERS client id") {
    const std::string src = read_repo_file("/terminal/src/app.cpp");
    // The exact expression that caused the incident, and any respelling of the
    // 20-39 rotation. `tws_client_seq_` no longer exists at all: the feed has
    // its own counter, so a future edit cannot reach for a shared one by habit.
    CHECK(src.find("tws_client_seq_") == std::string::npos);
    CHECK(src.find("20 + (") == std::string::npos);
    CHECK(src.find("% 20)") == std::string::npos);
    // ...and the orders id comes from the constant.
    CHECK(src.find("tc.client_id = kTwsOrdersClientId;") != std::string::npos);
}

TEST_CASE("the old broker is reaped BEFORE the new one is constructed") {
    // TwsBroker's constructor spawns its I/O thread and connects immediately, so
    // with a fixed client id a leftover adapter still holding id 20 is a
    // guaranteed self-collision. The reap used to sit ~200 lines further down,
    // after the new broker was already dialling.
    const std::string src = read_repo_file("/terminal/src/app.cpp");
    const size_t reap = src.find("reap_async(std::move(tws_), broker_reaps_);");
    const size_t make = src.find("tws_broker = std::make_unique<TwsBroker>");
    REQUIRE(reap != std::string::npos);
    REQUIRE(make != std::string::npos);
    CHECK(reap < make);
}

TEST_CASE("the lineup swap waits for its own socket to close before restarting") {
    // The swap restarts on the NEXT FRAME (~16 ms) after handing the old adapter
    // to a detached thread, and the old I/O thread can sit in waitForSignal for
    // up to a second before it notices stop_ and calls eDisconnect. Under a
    // fixed id that is a self-inflicted 326 on every swap.
    const std::string src = read_repo_file("/terminal/src/app.cpp");
    CHECK(src.find("broker_reaps_->load(std::memory_order_acquire) > 0 &&") !=
          std::string::npos);
    CHECK(src.find("swap_reap_deadline_s_") != std::string::npos);
}

// The text between two anchors. Bounding a source audit by a LENGTH makes it
// silently shrink as comments are added around the code it is auditing — which
// is how the openOrder window below stopped covering the foreign-order check
// while still passing everything it could still see.
static std::string between(const std::string& src, const char* from,
                           const char* to) {
    const size_t a = src.find(from);
    REQUIRE_MESSAGE(a != std::string::npos, "anchor not found: " << from);
    const size_t b = src.find(to, a);
    REQUIRE_MESSAGE(b != std::string::npos, "end anchor not found: " << to);
    return src.substr(a, b - a);
}

TEST_CASE("adoption advances the order-id counter, and checks who placed it") {
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    // Both live in openOrder, the only place this process learns about order ids
    // it did not issue.
    const std::string body =
        between(src, "void openOrder(OrderId orderId", "void openOrderEnd()");
    CHECK(body.find("tws_advance_order_id(next_tws_id, static_cast<long>(orderId) + 1)") !=
          std::string::npos);
    CHECK(body.find("order.clientId != b.cfg_.client_id") != std::string::npos);
    CHECK(body.find("tws_foreign_order_line") != std::string::npos);
    // ...and BOTH of them ahead of the symbol filter. An id spent on a ticker
    // today's lineup does not trade is exactly as spent as one on a ticker it
    // does, so an early return above the advance reopens Mechanism B for every
    // resting order outside the lineup — and a lineup swap is precisely what
    // changes that list. The advance sat after this return under a comment
    // claiming it did not.
    const size_t advance =
        body.find("tws_advance_order_id(next_tws_id, static_cast<long>(orderId) + 1)");
    const size_t foreign = body.find("tws_foreign_order_line");
    const size_t sid_filter = body.find("if (sid == 0) return;");
    REQUIRE(sid_filter != std::string::npos);
    CHECK(advance < sid_filter);
    CHECK(foreign < sid_filter);
    // nextValidId must not assign outright — that is what let a reconnect or a
    // reqIds answer pull the counter back below the account's high-water mark.
    CHECK(src.find("next_tws_id = orderId;") == std::string::npos);
    // 103 must be fatal, or the engine waits on a refused order forever.
    CHECK(src.find("case 103:") != std::string::npos);
}

TEST_CASE("the position audit seeds nothing") {
    // Rule 1 of net/book_divergence.h: it detects, it does not fix. A drift
    // detector that silently re-seeded the portfolio would repair the number and
    // erase the evidence that the order path had gone deaf.
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    const std::string body =
        between(src, "if (audit_active) {", "if (!recon_active) return;");
    CHECK(body.find("push_ev") == std::string::npos);
    CHECK(body.find("PosSnap") == std::string::npos);
    CHECK(body.find("net_pos") == std::string::npos);
}

TEST_CASE("the position audit reports positions OUTSIDE the lineup") {
    // Otherwise DivergeKind::BrokerOnly is unreachable on the live wire: the
    // app-side book is built from the session symbol list, so if the broker side
    // is filtered by the SAME list its symbol set is always a subset and the
    // second pass of compare_books can never fire. That is the direction
    // net/book_divergence.h calls the dangerous one — a real position with no
    // strategy and no stop — and it is how the 2026-08-06 orphan that cost $846
    // escaped: the lineup swap dropped the symbol, its close never filled, and
    // the next session's audit could no longer see it.
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    const std::string body =
        between(src, "if (audit_active) {", "if (!recon_active) return;");
    // The unconditional "not a session symbol -> drop the row" is gone; what is
    // left drops only a ZERO row on an off-lineup symbol (IB reports closed
    // positions as 0 all day, and those are evidence of nothing).
    CHECK(body.find("if (sid == 0) return;") == std::string::npos);
    // What survives filters only rows that cannot be an orphan THIS app made:
    // a flat symbol (IB reports a closed position as 0 all day) and a non-stock
    // row (reqPositions also reports CASH rows for an FX balance).
    CHECK(body.find("if (sid == 0 && (qty == 0.0 || contract.secType != \"STK\")) return;") !=
          std::string::npos);
    // Off-lineup rows are reported under IB's own spelling; session symbols keep
    // the session table's, so that side stays string-identical by construction.
    CHECK(body.find("sid ? b.cfg_.symbols[sid - 1] : contract.symbol") !=
          std::string::npos);
}

TEST_CASE("a placement is gated on the HANDSHAKE, not on the id sentinel") {
    // `next_tws_id < 0` doubled as the handshake gate only because
    // drop_connection reset it. It is now the account's order-id high-water mark
    // and is deliberately kept across a reconnect, so after the first successful
    // connect it is >= 0 forever and the only remaining gate was `!client` —
    // which goes true the instant connect_gateway() returns, BEFORE nextValidId
    // and before an incoming 326 has been read off the wire. An order written
    // into that window is never acked and never rejected: the engine leaves it
    // Working forever, and if it was a bracket's stop leg the app reports the
    // symbol protected while nothing rests at the broker.
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    CHECK(src.find("next_tws_id < 0 || !client") == std::string::npos);
    CHECK(src.find("if (!order_path_ready())") != std::string::npos);
    const std::string gate =
        between(src, "bool order_path_ready() const {", "void handle_submit(");
    CHECK(gate.find("handshaked") != std::string::npos);
    CHECK(gate.find("b.ready_.load") != std::string::npos);
}

TEST_CASE("a reconcile that never finishes cannot disable the auditor") {
    // start_audit() returns early while recon_active is true, and NOTHING else
    // cleared it: a socket drop mid-reqAllOpenOrders, or a missing
    // accountSummaryEnd, turned the position auditor off for the whole session
    // while the engine's own 10 s failsafe armed it anyway. /diag then read
    // "unknown: no broker answer for Ns" all day with the phantom detector dead.
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    // (a) the disconnect path clears it...
    const std::string drop =
        between(src, "void drop_connection() {", "// ---- EWrapper callbacks");
    CHECK(drop.find("recon_active = false;") != std::string::npos);
    // ...and lets the reconnect run the reconcile it never completed.
    CHECK(drop.find("recon_ever = false;") != std::string::npos);
    // (b) and a socket that stays UP but stops answering has a deadline.
    CHECK(src.find("void check_reconcile_timeout()") != std::string::npos);
    CHECK(src.find("io.check_reconcile_timeout();") != std::string::npos);
    CHECK(src.find("kReconcileTimeoutMs") != std::string::npos);
}

TEST_CASE("the direct start path WAITS for its own socket to close") {
    // Moving the reap above the construction was not enough: reap_async detaches
    // and returns immediately, so the next statement dialled the fixed orders
    // client id while the old adapter still held it (~TwsBroker does not poke the
    // reader signal, so its I/O thread waits out waitForSignal's 1 s timeout
    // first). Only the lineup swap waited. The scheduled 15:55 stop went through
    // Engine::stop_live rather than App::safe_stop_live, so the adapter survived
    // the night and 09:25's auto-start hit exactly this.
    const std::string src = read_repo_file("/terminal/src/app.cpp");
    const size_t wait = src.find("wait_for_broker_reap(\"live\");");
    const size_t make = src.find("tws_broker = std::make_unique<TwsBroker>");
    REQUIRE(wait != std::string::npos);
    REQUIRE(make != std::string::npos);
    CHECK(wait < make);
    // ...and the scheduled stop reaps the broker at 15:55, not at 09:25.
    const std::string sched = read_repo_file("/terminal/src/panels/trade.cpp");
    CHECK(sched.find("if (stop) stop();") != std::string::npos);
    CHECK(src.find("[this] { safe_stop_live(); });") != std::string::npos);
}
