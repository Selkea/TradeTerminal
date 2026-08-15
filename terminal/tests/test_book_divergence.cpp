// Book-divergence detection: does the app's book still match the ACCOUNT's?
//
// On 2026-08-13 an adopted take-profit filled at the broker and the fill went to
// a TWS client id that no longer existed. The app carried a PHANTOM 411-share
// position for 4 h 03 m, published a fictional +$528 unrealized and reported the
// symbol "protected". Nothing noticed, because there is no callback for a
// callback that never arrives.
//
// These cases pin the three properties that make the detector able to see that,
// and able to see its own failure:
//   - a disagreement in either direction is reported, named, with both numbers,
//   - an ordinary fill mid-comparison does NOT produce an alarm, by two
//     independent mechanisms, and
//   - the metric's healthy value is unreachable while the auditor is broken.
#include "doctest.h"

#include "net/book_divergence.h"

using namespace tt::net;

static constexpr int64_t kSec = 1000;
static constexpr int64_t kMin = 60 * kSec;

// ---- compare_books: the pure comparison ------------------------------------

TEST_CASE("compare: identical books diverge about nothing") {
    const std::vector<BookPos> app{{"RAM", 411}, {"MUU", -200}, {"SNXX", 0}};
    const std::vector<BookPos> broker{{"RAM", 411}, {"MUU", -200}, {"SNXX", 0}};
    CHECK(compare_books(app, broker).empty());
}

TEST_CASE("compare: flat on both sides is agreement, not an entry") {
    // The app's book always lists every session symbol, most of them flat. If a
    // zero-vs-absent pair counted as a divergence, every idle session would page.
    const std::vector<BookPos> app{{"RAM", 0}, {"MUU", 0}};
    CHECK(compare_books(app, {}).empty());
    CHECK(compare_books(app, {{"RAM", 0}}).empty());
}

TEST_CASE("compare: quantity mismatch names both numbers") {
    // A partial fill the app never heard about: 411 sold down to 200.
    const auto d = compare_books({{"RAM", 411}}, {{"RAM", 200}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].symbol == "RAM");
    CHECK(d[0].app_qty == 411);
    CHECK(d[0].broker_qty == 200);
    CHECK(d[0].kind == DivergeKind::Quantity);
}

TEST_CASE("compare: a sign flip is labelled Sign, not Quantity") {
    // Long 400 where the account is short 400 is a different morning from
    // long 400 vs long 411, and the label is what the operator reads at 03:00.
    const auto d = compare_books({{"RAM", 400}}, {{"RAM", -400}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].kind == DivergeKind::Sign);
    const auto e = compare_books({{"RAM", -400}}, {{"RAM", 400}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == DivergeKind::Sign);
}

TEST_CASE("compare: flat-vs-held is not a sign flip") {
    // Zero has no direction. Calling 0-vs-100 a "sign" divergence would put the
    // scariest word on the most ordinary shape of the bug.
    const auto d = compare_books({{"RAM", 0}}, {{"RAM", 100}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].kind == DivergeKind::Quantity);
}

TEST_CASE("compare: THE 2026-08-13 PHANTOM — app holds it, the broker does not") {
    // The exact shape of the incident: the app's book says 411 shares of RAM,
    // the account says nothing at all. Selling this would open a naked short.
    const auto d = compare_books({{"RAM", 411}, {"MUU", 0}}, {});
    REQUIRE(d.size() == 1);
    CHECK(d[0].symbol == "RAM");
    CHECK(d[0].app_qty == 411);
    CHECK(d[0].broker_qty == 0);
    CHECK(d[0].kind == DivergeKind::AppOnly);
}

TEST_CASE("compare: the broker holds a symbol the book does not list at all") {
    // The dangerous inverse — a REAL position with no strategy and no stop, the
    // 2026-08-06 orphan. It must be reported even though the app's book has no
    // row for it, which is why the second pass over `broker` exists.
    const auto d = compare_books({{"MUU", 0}}, {{"SNXX", -300}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].symbol == "SNXX");
    CHECK(d[0].app_qty == 0);
    CHECK(d[0].broker_qty == -300);
    CHECK(d[0].kind == DivergeKind::BrokerOnly);
}

TEST_CASE("compare: sub-epsilon noise is not a divergence, one share is") {
    // Quantities travel as doubles through IB's Decimal and the portfolio, so a
    // tolerance is needed — but a one-share disagreement is a real one.
    CHECK(compare_books({{"RAM", 411.0}}, {{"RAM", 411.0 + 1e-9}}).empty());
    CHECK(compare_books({{"RAM", 411}}, {{"RAM", 410}}).size() == 1);
}

TEST_CASE("compare: output is sorted, so /diag does not reshuffle between rounds") {
    const auto d = compare_books({{"SNXX", 1}, {"AAOX", 2}, {"MUU", 3}}, {});
    REQUIRE(d.size() == 3);
    CHECK(d[0].symbol == "AAOX");
    CHECK(d[1].symbol == "MUU");
    CHECK(d[2].symbol == "SNXX");
}

// ---- the in-flight-fill race, mechanism 1: settling ------------------------

TEST_CASE("race: a symbol that moved DURING the audit is dropped from the round") {
    // The broker's snapshot was taken when the app held 411; by the time it
    // landed a fill had taken the app to 0. Comparing two books captured at
    // different instants proves nothing, so this round says nothing about RAM —
    // while still judging every other symbol.
    const auto d = compare_books({{"RAM", 0}, {"MUU", 500}}, {{"RAM", 411}}, {"RAM"});
    REQUIRE(d.size() == 1);
    CHECK(d[0].symbol == "MUU");
}

TEST_CASE("race: settling suppresses the broker-side pass too") {
    // Otherwise a symbol excluded on the app side would come straight back
    // through the BrokerOnly sweep, and the exclusion would be decorative.
    CHECK(compare_books({}, {{"RAM", 411}}, {"RAM"}).empty());
}

// ---- the in-flight-fill race, mechanism 2: confirmation --------------------

TEST_CASE("race: one sighting never pages") {
    // The dangerous transient — the broker has filled the exit and reported it,
    // the engine has not drained the event yet — looks EXACTLY like the phantom
    // at this instant. Nothing in a single snapshot can tell them apart.
    BookAudit a;
    a.arm(0);
    const auto confirmed = a.observe({{"RAM", 411}}, {}, {}, 0);
    CHECK(confirmed.empty());
    CHECK(a.state() == AuditState::Suspect);
}

TEST_CASE("race: the same disagreement a minute later IS confirmed") {
    // What separates the transient from the bug is time: the engine drains its
    // event ring every frame, so a real in-flight fill cannot still be
    // outstanding 60s later. A callback that is never coming can.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {}, {}, 0);
    const auto confirmed = a.observe({{"RAM", 411}}, {}, {}, kMin);
    REQUIRE(confirmed.size() == 1);
    CHECK(confirmed[0].symbol == "RAM");
    CHECK(confirmed[0].app_qty == 411);
    CHECK(confirmed[0].broker_qty == 0);
    CHECK(confirmed[0].rounds == 2);
    CHECK(a.state() == AuditState::Diverged);
    CHECK(a.confirmed_span_ms() == kMin);
}

TEST_CASE("race: an in-flight fill resolving on the next round never pages") {
    // The full transient, played out: round 1 sees 411-vs-0, round 2 sees the
    // engine has caught up and both are flat. No alert was ever raised.
    BookAudit a;
    a.arm(0);
    CHECK(a.observe({{"RAM", 411}}, {}, {}, 0).empty());
    CHECK(a.observe({{"RAM", 0}}, {}, {}, kMin).empty());
    CHECK(a.state() == AuditState::Ok);
}

TEST_CASE("race: a book still in MOTION resets the streak instead of confirming") {
    // A large order filling in stages moves the app side every round. Three
    // rounds of disagreement, never the same disagreement twice — that is a
    // position being worked, not a book that has diverged.
    BookAudit a;
    a.arm(0);
    CHECK(a.observe({{"RAM", 411}}, {{"RAM", 300}}, {}, 0).empty());
    CHECK(a.observe({{"RAM", 411}}, {{"RAM", 380}}, {}, kMin).empty());
    CHECK(a.observe({{"RAM", 411}}, {{"RAM", 400}}, {}, 2 * kMin).empty());
    CHECK(a.state() == AuditState::Suspect);
    // ...and when it finally settles wrong, it takes two matching rounds again.
    CHECK(a.observe({{"RAM", 411}}, {{"RAM", 400}}, {}, 3 * kMin).size() == 1);
}

TEST_CASE("race: a divergence that clears returns the auditor to ok") {
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {}, {}, 0);
    REQUIRE(a.observe({{"RAM", 411}}, {}, {}, kMin).size() == 1);
    CHECK(a.observe({{"RAM", 411}}, {{"RAM", 411}}, {}, 2 * kMin).empty());
    CHECK(a.state() == AuditState::Ok);
    CHECK(a.confirmed().empty());
}

// ---- the metric must be able to see its OWN failure -------------------------
//
// This project's recurring defect is instrumentation that reads healthy while
// broken: oldest_history_age_ms pinned at 0 through a five-hour outage,
// data.connected true against a login modal, stuck_orders at 2 with nothing
// watching. These are the cases that say this one cannot do that.

TEST_CASE("metric: an unarmed auditor says off, and claims no age") {
    BookAudit a;
    CHECK(a.state() == AuditState::Off);
    CHECK(a.field(0) == "off");
    // -1, not 0. Reporting "0 ms since agreement" for a session that has never
    // audited anything is the precise lie oldest_history_age_ms told.
    CHECK(a.last_agreed_age_ms(5 * kMin) == -1);
}

TEST_CASE("metric: armed but unanswered says starting, NOT ok") {
    BookAudit a;
    a.arm(0);
    CHECK(a.field(0) == "starting");
    CHECK(a.tick(kSec) == AuditState::Starting);
}

TEST_CASE("metric: an armed auditor that is NEVER answered ages from arming") {
    // The gauge's whole job is to climb in every failure mode, and it did not
    // climb in the one it was chosen for. An auditor armed and then never
    // answered — a reconcile that wedged start_audit for the session — reported
    // -1 forever, which is BELOW every `>` threshold a remote rule can write,
    // while tt_book_divergences sat at 0. Both gauges read benign for the whole
    // trading day with the phantom detector dead: oldest_history_age_ms pinned
    // at 0 through a five-hour outage, again.
    BookAudit a;
    a.arm(0);
    CHECK(a.last_agreed_age_ms(kSec) == kSec);
    CHECK(a.last_agreed_age_ms(30 * kMin) == 30 * kMin);   // it CLIMBS
    // ...and a rule of the documented shape actually fires.
    CHECK(a.last_agreed_age_ms(30 * kMin) > 5 * kMin);
    // -1 survives for the only genuinely unmeasurable case: no live session.
    // That is what keeps a Saturday scrape distinguishable from a broken one.
    a.stand_down();
    CHECK(a.last_agreed_age_ms(30 * kMin) == -1);
}

TEST_CASE("a round that compared NOTHING is not agreement") {
    // seen.empty() had two meanings — "every symbol agreed" and "there was
    // nothing to look at" — and recorded them the same way. A one-symbol lineup
    // whose only symbol is excluded as `settling` (a fill landed between the
    // request and the answer) therefore flipped /diag to "ok" and reset the age
    // on a comparison that examined zero positions.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {}, {}, kMin);
    a.observe({{"RAM", 411}}, {}, {}, 2 * kMin);
    REQUIRE(a.state() == AuditState::Diverged);
    // Now a round in which the only symbol is settling. It carries no evidence.
    a.observe({{"RAM", 411}}, {{"RAM", 0}}, {"RAM"}, 3 * kMin);
    CHECK(a.state() == AuditState::Diverged);            // NOT reset to Ok
    CHECK(a.field(3 * kMin) != "ok");
    CHECK(a.last_agreed_age_ms(3 * kMin) == 3 * kMin);   // the age kept climbing
    // It still counts as an ANSWER, though: the auditor is being served, so it
    // must not also start claiming it is blind.
    CHECK(a.unanswered_ms(3 * kMin) == 0);
    // comparable_count is the discriminator, and it counts the broker-only
    // direction too — that side of the comparison is the dangerous one.
    CHECK(comparable_count({{"RAM", 411}}, {{"RAM", 0}}, {"RAM"}) == 0);
    CHECK(comparable_count({}, {{"SNXX", 300}}, {}) == 1);
    CHECK(comparable_count({{"RAM", 0}}, {}, {}) == 1);
}

// ---- the alert latch --------------------------------------------------------

TEST_CASE("the alert is cleared ONLY by an audit that agreed") {
    // WatchdogTimer answers update(false) after an alert with Recovered, so
    // whatever feeds it decides what the operator is told. Feeding it
    // `state == Diverged` meant BOTH exits from Diverged read as recovery.
    DivergenceLatch l;
    const std::vector<Divergence> d{{"RAM", 411, 0, DivergeKind::AppOnly, 2}};
    l.update(AuditState::Diverged, d, kMin);
    REQUIRE(l.open());
    CHECK(l.span_ms() == kMin);

    // (a) the quantities MOVE — a resting exit the app is deaf to, filling in
    // stages. The streak resets, confirmed() empties, the state drops to
    // Suspect. The books have not agreed; this alternated PAGE / "they agree
    // again" / PAGE every 60 s.
    l.update(AuditState::Suspect, {}, 0);
    CHECK(l.open());
    CHECK(l.divergences().size() == 1);       // still names the symbol to page about
    CHECK(l.divergences()[0].symbol == "RAM");

    // (b) answers stop entirely. Blind is the least recovered a detector can be,
    // and on this route the compensating BOOK AUDIT BLIND page is suppressed
    // when the order socket is down — so a false all-clear here was the last
    // word the operator got about a live phantom.
    l.update(AuditState::Blind, {}, 0);
    CHECK(l.open());

    // ...and the one thing that does clear it.
    l.update(AuditState::Ok, {}, 0);
    CHECK_FALSE(l.open());
    CHECK(l.span_ms() == 0);
}

TEST_CASE("the latch belongs to a session, and starts empty") {
    DivergenceLatch l;
    CHECK_FALSE(l.open());
    l.update(AuditState::Starting, {}, 0);   // no evidence either way
    CHECK_FALSE(l.open());
    l.update(AuditState::Diverged, {{"SNXX", 0, 300, DivergeKind::BrokerOnly, 2}},
             2 * kMin);
    REQUIRE(l.open());
    // stand-down is not a recovery: it clears silently, so the next session
    // starts from nothing rather than inheriting a page.
    l.clear();
    CHECK_FALSE(l.open());
}

TEST_CASE("metric: an auditor nobody answers goes BLIND on its own") {
    // The killer property. tick() is called every frame whether or not an answer
    // came back, so a detector that has stopped being served degrades by itself
    // instead of sitting on the last ok it happened to record.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {{"RAM", 411}}, {}, kMin);
    CHECK(a.state() == AuditState::Ok);
    CHECK(a.tick(2 * kMin) == AuditState::Ok);          // one missed round: fine
    CHECK(a.tick(kMin + kBookAuditBlindMs) == AuditState::Ok);   // exactly at the edge
    CHECK(a.tick(kMin + kBookAuditBlindMs + 1) == AuditState::Blind);
    CHECK(a.field(kMin + kBookAuditBlindMs + 1).rfind("unknown:", 0) == 0);
    CHECK(a.blind());
}

TEST_CASE("metric: the agreed-age climbs in EVERY failure mode") {
    // The one number that means the same thing in all of them. It is reset only
    // by positive evidence — an answer that came back AND agreed.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 0}}, {{"RAM", 0}}, {}, kMin);
    CHECK(a.last_agreed_age_ms(kMin) == 0);
    // (1) the books disagree: answers keep arriving, the age keeps climbing.
    a.observe({{"RAM", 411}}, {}, {}, 2 * kMin);
    a.observe({{"RAM", 411}}, {}, {}, 3 * kMin);
    CHECK(a.last_agreed_age_ms(3 * kMin) == 2 * kMin);
    // (2) nothing answers at all: same direction, no special case needed.
    CHECK(a.last_agreed_age_ms(30 * kMin) == 29 * kMin);
}

TEST_CASE("metric: ok is unreachable without a fresh AGREEING answer") {
    // The whole contract in one case. There is no path from Blind or Diverged
    // back to Ok except through an audit that came back and matched.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {}, {}, 0);
    a.observe({{"RAM", 411}}, {}, {}, kMin);
    REQUIRE(a.state() == AuditState::Diverged);
    a.tick(kMin + kBookAuditBlindMs + 1);
    REQUIRE(a.state() == AuditState::Blind);
    CHECK(a.field(kMin + kBookAuditBlindMs + 1) != "ok");
    a.observe({{"RAM", 411}}, {{"RAM", 411}}, {}, 20 * kMin);
    CHECK(a.field(20 * kMin) == "ok");
}

TEST_CASE("metric: stand_down clears the session, it does not carry ok forward") {
    // Freshness belongs to a SESSION. The terminal stays up across the nightly
    // 15:55 stop / 09:25 start, so an "ok" from yesterday must not stand as
    // evidence about this morning's book.
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {{"RAM", 411}}, {}, 0);
    REQUIRE(a.state() == AuditState::Ok);
    a.stand_down();
    CHECK(a.state() == AuditState::Off);
    CHECK(a.last_agreed_age_ms(kMin) == -1);
    CHECK(a.rounds() == 0);
    a.arm(20 * kMin);
    CHECK(a.field(20 * kMin) == "starting");
}

// ---- the operator-facing strings -------------------------------------------

TEST_CASE("text: the /diag field names the symbol and both quantities") {
    BookAudit a;
    a.arm(0);
    a.observe({{"RAM", 411}}, {}, {}, 0);
    const std::string suspect = a.field(0);
    CHECK(suspect.find("suspect") != std::string::npos);
    CHECK(suspect.find("RAM") != std::string::npos);
    CHECK(suspect.find("app=411") != std::string::npos);
    CHECK(suspect.find("broker=0") != std::string::npos);
    CHECK(suspect.find("(1/2)") != std::string::npos);
    a.observe({{"RAM", 411}}, {}, {}, kMin);
    const std::string diverged = a.field(kMin);
    CHECK(diverged.rfind("DIVERGED:", 0) == 0);
    CHECK(diverged.find("app=411") != std::string::npos);
}

TEST_CASE("text: whole shares read as whole shares") {
    // This string ends up in a phone alert. std::to_string(411.0) is
    // "411.000000", which is how a page stops being read.
    CHECK(describe_divergence({"RAM", 411, 0, DivergeKind::AppOnly, 2}) ==
          "RAM app=411 broker=0 (app-only)");
    CHECK(describe_divergence({"MUU", -200, -50, DivergeKind::Quantity, 2}) ==
          "MUU app=-200 broker=-50 (quantity)");
}

TEST_CASE("text: the page carries the tag, the symbol, both numbers and the window") {
    const std::string msg =
        book_divergence_alert({{"RAM", 411, 0, DivergeKind::AppOnly, 2}}, 61 * kSec);
    CHECK(msg.rfind(kBookDivergenceTag, 0) == 0);   // classify_alert pages on this
    CHECK(msg.find("RAM app=411 broker=0") != std::string::npos);
    CHECK(msg.find("61s") != std::string::npos);    // the claim that rules out a fill
    CHECK(msg.find("NAKED SHORT") != std::string::npos);
}

TEST_CASE("text: the blind page says it is NOT an all-clear") {
    const std::string msg = book_audit_blind_alert(312 * kSec);
    CHECK(msg.rfind(kBookAuditBlindTag, 0) == 0);
    CHECK(msg.find("312s") != std::string::npos);
    CHECK(msg.find("no information") != std::string::npos);
    // ...and it is a DIFFERENT tag from a divergence, so a blind auditor and a
    // real disagreement can never be confused in the alert channel.
    CHECK(std::string(kBookAuditBlindTag) != std::string(kBookDivergenceTag));
    CHECK(msg.find(kBookDivergenceTag) == std::string::npos);
}

// ---- cadence ---------------------------------------------------------------

TEST_CASE("cadence: a divergence is caught in minutes, not hours") {
    // The requirement, as arithmetic. Two rounds at the audit interval is the
    // worst case from onset to page; the 2026-08-13 phantom lived 4 h 03 m.
    const int64_t worst_case_ms = kBookAuditIntervalMs * kBookAuditConfirmRounds;
    CHECK(worst_case_ms <= 5 * kMin);
    // ...and the auditor's own death is visible on the same scale.
    CHECK(kBookAuditBlindMs <= 10 * kMin);
    CHECK(kBookAuditBlindMs > kBookAuditIntervalMs);   // one missed round is not an event
}

// ---------------------------------------------------------------------------
// THE BROKER'S BOOK, KEPT — because nothing else in the app has it.
//
// Connect-time reconciliation cannot answer "what does the broker actually
// hold": it discards every position outside cfg_.symbols, above its own log line
// (engine/reconcile_policy.h — 20 NVDA shares invisible for 24 days). The
// auditor is the only place the whole answer arrives, so App::begin_lineup_swap
// reads it to decide whether it may drop a symbol.
//
// It used to decide that from snap.symbols[i].position.qty — the APP's own book,
// which is precisely the number this entire class of bug corrupts. On 2026-08-13
// that number was a phantom for four hours. On 2026-08-06 a dropped symbol's
// market-close never filled and the $846 position it left behind was invisible
// to every session afterwards: outside cfg.symbols a position can be neither
// adopted, nor audited, nor flattened, nor reached by the 15:57 EOD backstop.

TEST_CASE("audit: the broker's whole book survives the comparison") {
    BookAudit a;
    a.arm(0);
    CHECK(a.last_broker().empty());   // nothing has answered yet

    // The 2026-08-14 shape: the app trades MUU and SOXS, and the account also
    // holds 20 NVDA that no lineup has touched since 2026-07-21.
    a.observe({{"MUU", 0}, {"SOXS", 100}},
              {{"MUU", 0}, {"SOXS", 100}, {"NVDA", 20}}, {}, kMin);
    REQUIRE(a.last_broker().size() == 3);

    bool unknown = true;
    CHECK(a.broker_holds("SOXS", unknown));
    CHECK_FALSE(unknown);
    CHECK_FALSE(a.broker_holds("MUU", unknown));   // present and flat
    CHECK_FALSE(unknown);
    // Kept WHOLE, before the comparison can discard it: NVDA is not in the app's
    // book at all, so any storage keyed off the app side would lose exactly the
    // row that matters.
    CHECK(a.broker_holds("NVDA", unknown));
    CHECK_FALSE(unknown);
    // A symbol the broker never mentioned is flat, and that answer is KNOWN —
    // the answer arrived, and it did not name this symbol.
    CHECK_FALSE(a.broker_holds("KORU", unknown));
    CHECK_FALSE(unknown);
}

TEST_CASE("audit: silence is not flatness") {
    // The distinction the caller must be able to make. Treating "nobody has
    // asked" as "the broker says flat" is the assumption that produced the
    // 2026-08-06 orphan, and folding unknown into false would hide it again.
    BookAudit a;
    a.arm(0);
    bool unknown = false;
    CHECK_FALSE(a.broker_holds("SOXS", unknown));
    CHECK(unknown);

    // A round that compares NOTHING must not be mistaken for evidence either.
    a.observe({}, {}, {}, kMin);
    CHECK(a.last_broker().empty());
    unknown = false;
    CHECK_FALSE(a.broker_holds("SOXS", unknown));
    CHECK(unknown);
}

TEST_CASE("audit: the broker's book belongs to a session, not to the process") {
    // stand_down() runs when the session ends. A book carried across it would
    // let yesterday's positions decide whether today's lineup may drop a symbol.
    BookAudit a;
    a.arm(0);
    a.observe({{"MUU", 0}}, {{"MUU", 0}, {"NVDA", 20}}, {}, kMin);
    bool unknown = true;
    REQUIRE(a.broker_holds("NVDA", unknown));
    a.stand_down();
    CHECK(a.last_broker().empty());
    unknown = false;
    CHECK_FALSE(a.broker_holds("NVDA", unknown));
    CHECK(unknown);
}

// ---- the gate that could never refuse --------------------------------------
//
// start_live_session refuses a symbol whose time stop cannot fire within one
// session (the 2026-08-14 STKH failure: time_stop 233 bars against 78 bars of
// RTH, no price stop, and a z-exit gated on being above entry — so nothing could
// close it at a loss, and it fell 10.1%). It carves out "unless the broker might
// still be holding it", so refusing cannot orphan a real position.
//
// That gate runs BETWEEN sessions, after stand_down() has cleared the live book.
// Reading the live book there made `unknown` unconditionally true, so the
// carve-out always fired and the gate never refused anything at all.

TEST_CASE("stand_down clears the live book, so broker_holds cannot answer") {
    BookAudit a;
    a.arm(1000);
    a.observe({}, {{"STKH", 0.0}, {"NVDA", 20.0}}, {}, 2000);
    bool unknown = true;
    CHECK(a.broker_holds("NVDA", unknown));
    CHECK_FALSE(unknown);
    a.stand_down();
    // The live accessor must still go blank — divergence pages must never be
    // raised off a dead session's book.
    CHECK_FALSE(a.broker_holds("NVDA", unknown));
    CHECK(unknown);
}

TEST_CASE("the between-sessions accessor survives stand_down") {
    BookAudit a;
    a.arm(1000);
    a.observe({}, {{"STKH", 0.0}, {"NVDA", 20.0}}, {}, 2000);
    a.stand_down();
    bool unknown = true;
    // Held last session -> the start gate keeps the symbol rather than orphaning it.
    CHECK(a.broker_held_last_session("NVDA", unknown));
    CHECK_FALSE(unknown);
    // Confirmed FLAT last session -> `unknown` is false, so `holds || unknown`
    // is false and the unreachable-time-stop symbol is actually REFUSED. This is
    // the assertion that fails against the shipped version.
    CHECK_FALSE(a.broker_held_last_session("STKH", unknown));
    CHECK_FALSE(unknown);
}

TEST_CASE("with no answer ever, silence is still not flat") {
    // The opposite error, and the more dangerous one: treating "nobody has
    // asked" as "the broker says flat" is what orphaned a live position on
    // 2026-08-06. A fresh process must report unknown, so the caller keeps the
    // symbol.
    BookAudit a;
    bool unknown = false;
    CHECK_FALSE(a.broker_held_last_session("STKH", unknown));
    CHECK(unknown);
    a.arm(1000);
    a.stand_down();   // stood down without ever having received a book
    unknown = false;
    CHECK_FALSE(a.broker_held_last_session("STKH", unknown));
    CHECK(unknown);
}
