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
    CHECK(a.last_agreed_age_ms(kSec) == -1);
    CHECK(a.tick(kSec) == AuditState::Starting);
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
