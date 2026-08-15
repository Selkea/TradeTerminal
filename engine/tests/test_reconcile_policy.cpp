// What connect-time reconciliation does with a broker position it does not
// recognise — the 24-day blind spot, found 2026-08-14.
//
// The account held 20 NVDA shares from 2026-07-21 (the late second partial of a
// 120-share entry whose exit had already been sized off the first 100). NVDA
// left the lineup on 2026-07-24. IB sent the row on EVERY reconcile after that,
// and TwsIo::position dropped it with a bare `if (sid == 0) return;` placed
// ABOVE the "reconcile: position row" log — so it was not adopted, not
// accumulated, and not even logged. The log could not distinguish "IB never sent
// it" from "we threw it away".
//
// It was proven three separate times on 2026-08-13/14: a reconcile printed its
// rows and named no NVDA, and the position AUDITOR — the same
// client->reqPositions() on the same socket, one second later — reported
// NVDA app=0 broker=20. The auditor could see it only because 0.22.1
// deliberately opened its half of the callback to off-lineup rows; the reconcile
// half was left as it had been since the original hot-restart commit.
//
// And it is worse than invisible: TwsBroker::flatten walks net_pos by symbol id,
// so cancel-all, flatten, the kill switch and the 15:57 EOD backstop cannot
// reach an off-lineup position at all.
//
// The classification is a pure function of the three facts the callback has, so
// it is driven directly here. The call site itself lives inside a TWS callback
// that needs a gateway to execute, so it is pinned as source text at the bottom
// — the same technique test_tws_order_identity.cpp uses for the client id.
#include "doctest.h"

#include "engine/events.h"
#include "engine/reconcile_policy.h"

#include <fstream>
#include <iterator>
#include <string>

using namespace tt;

TEST_CASE("reconcile: a session symbol is adopted, exactly as before") {
    CHECK(classify_reconcile_row(1, 20, "STK") == ReconRow::Adopt);
    CHECK(classify_reconcile_row(3, -411, "STK") == ReconRow::Adopt);
    // A session symbol is adopted on its sid alone. IB reports a closed position
    // as 0 for the rest of the day, and reconciliation MUST see that zero: it is
    // what seeds net_pos and what tells the engine the symbol is flat.
    CHECK(classify_reconcile_row(1, 0, "STK") == ReconRow::Adopt);
    // ...and not on secType either. Filtering a session symbol by contract type
    // would silently un-adopt whatever this app is actually trading.
    CHECK(classify_reconcile_row(1, 20, "CASH") == ReconRow::Adopt);
}

TEST_CASE("reconcile: the NVDA row is REPORTED instead of silently dropped") {
    // The exact 2026-07-21 leftover, arriving on every reconcile from then until
    // 2026-08-13 and producing not one line of log.
    CHECK(classify_reconcile_row(0, 20, "STK") == ReconRow::OffLineupReport);
    CHECK(classify_reconcile_row(0, -20, "STK") == ReconRow::OffLineupReport);
    // The 2026-08-06 orphan is the same shape: a lineup swap dropped the symbol,
    // its market-close never filled, and $846 of stock became unreachable.
    CHECK(classify_reconcile_row(0, 411, "STK") == ReconRow::OffLineupReport);
}

TEST_CASE("reconcile: an off-lineup row that is not evidence stays quiet") {
    // Zero. IB reports a closed position as 0 for the rest of the day, and a
    // flat symbol we do not trade is not evidence of anything.
    CHECK(classify_reconcile_row(0, 0, "STK") == ReconRow::OffLineupIgnore);
    // Non-stock. reqPositions also reports CASH rows for a non-USD FX balance
    // and whatever else the account holds; this app places STK orders only, so
    // an orphan it can have CREATED is a stock. Without this the page would fire
    // Critical, unclearably, about a currency balance — and an alert that is
    // wrong every day is how the alert that is right gets ignored.
    CHECK(classify_reconcile_row(0, 1234.56, "CASH") == ReconRow::OffLineupIgnore);
    CHECK(classify_reconcile_row(0, 2, "OPT") == ReconRow::OffLineupIgnore);
    CHECK(classify_reconcile_row(0, 1, "FUT") == ReconRow::OffLineupIgnore);
    // Same two filters the AUDIT branch already applies, so the two halves of
    // one callback cannot disagree about what counts as a real position — which
    // is precisely how NVDA came to be visible to one and invisible to the
    // other for 24 days.
}

TEST_CASE("reconcile: the operator line says what NOTHING will do") {
    const std::string l = offlineup_position_line("NVDA", 20, 208.39);
    CHECK(l.find("NVDA") != std::string::npos);
    CHECK(l.find("OFF-LINEUP BROKER POSITION") != std::string::npos);
    // The fact that matters is not "a position was found" — "reconcile: position
    // row NVDA 20" would have been true for 24 days and would still have read as
    // routine. It is that this session cannot close it.
    CHECK(l.find("NOTHING here will close it") != std::string::npos);
    CHECK(l.find("flatten") != std::string::npos);
}

TEST_CASE("reconcile: ReconcileEnd can carry the count into the engine") {
    // The engine's "broker reconciliation complete - N symbol(s) held until
    // flat" line counted ADOPTED session symbols only, so it read "0 symbol(s)"
    // on an account holding 20 NVDA shares. An unrecognised symbol is never
    // adopted (PosSnap is keyed on a session symbol id and has no "other"
    // bucket), so the count is the only thing that can cross.
    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::ReconcileEnd);
    ev.u.recon.offlineup = 1;
    CHECK(ev.u.recon.offlineup == 1u);
    static_assert(sizeof(EngineEvent) == 64, "one cache line per event");
}

// ---- the call site, as source text ------------------------------------------
//
// TwsIo is defined inside tws_broker.cpp and needs a live gateway to run, so the
// wiring is checked by reading the file — the technique test_tws_order_identity
// uses to prove the client id has not started rotating again. It catches the
// regression that matters: someone restoring the bare early return.
static std::string read_repo_file(const char* rel) {
    const std::string path = std::string(TT_REPO_DIR) + rel;
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open " << path);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

TEST_CASE("reconcile: the TWS callback routes through the policy, not a bare return") {
    const std::string src = read_repo_file("/engine/src/tws_broker.cpp");
    CHECK(src.find("classify_reconcile_row") != std::string::npos);
    CHECK(src.find("offlineup_position_line") != std::string::npos);
    // Scoped to the RECONCILE arm of position(), between the guard that opens it
    // and the log line the discard used to sit above. openOrder() has its own,
    // deliberate `if (sid == 0) return;` — an off-lineup resting order is
    // already reported one line earlier by tws_foreign_order_line — and a
    // whole-file search would have banned that too.
    const size_t arm = src.find("if (!recon_active) return;");
    REQUIRE(arm != std::string::npos);
    const size_t row_log = src.find("reconcile: position row", arm);
    REQUIRE(row_log != std::string::npos);
    const std::string recon_arm = src.substr(arm, row_log - arm);
    // The line this replaced. Its comment ("nothing to adopt it into") was true
    // and was never the point: adoption was not the question, being TOLD was.
    CHECK(recon_arm.find("if (sid == 0) return;") == std::string::npos);
    CHECK(recon_arm.find("classify_reconcile_row") != std::string::npos);
    // The count must be published where the UI thread can read it for /diag, and
    // reset per reconcile so a position that has since been closed stops being
    // reported forever.
    CHECK(src.find("recon_offlineup_.store") != std::string::npos);
    CHECK(src.find("recon_offlineup = 0;") != std::string::npos);
}
