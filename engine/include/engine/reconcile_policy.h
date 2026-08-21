#pragma once
// WHAT CONNECT-TIME RECONCILIATION DOES WITH A BROKER POSITION IT DOES NOT
// RECOGNISE.
//
// The 24-day blind spot, found 2026-08-14.
//
// The account held 20 NVDA shares, bought 2026-07-21 at 09:30:26 as the second,
// late partial of a 120-share entry whose exit had already been sized off the
// first 100. NVDA left the lineup on 2026-07-24. From that day until 2026-08-13
// NOTHING in this application could see those shares:
//
//   - reqPositions is the only position ingress the TWS adapter has, and IB sent
//     the NVDA row on every single reconcile. TwsIo::position discarded it with
//     a bare `if (sid == 0) return;` — placed ABOVE the "reconcile: position row"
//     log, so it was not adopted, not accumulated, and NOT EVEN LOGGED. In the
//     log there is no difference between "IB never sent it" and "we threw it
//     away", and that indistinguishability is the defect: three separate
//     reconciles on 2026-08-13/14 printed their rows and none named NVDA, while
//     the position auditor — the SAME client->reqPositions() on the SAME socket,
//     one second later — reported NVDA app=0 broker=20 every time.
//   - the auditor could only see it because 0.22.1 deliberately opened its half
//     of this callback to off-lineup rows. The reconcile half was left as it
//     was, and it has been this way since the original hot-restart-reconciliation
//     commit. The position did not appear on 2026-08-13; the ABILITY TO SEE IT
//     did, 89 seconds after the first v0.22.1 process start.
//
// AND IT IS WORSE THAN INVISIBLE: TwsBroker::flatten walks net_pos indexed by
// symbol id, so cancel-all, flatten, the kill switch and the engine's 15:57 EOD
// backstop CANNOT close an off-lineup position at all. A symbol dropped from the
// lineup while holding stock stops being closeable and stops being auditable at
// the same instant — which is exactly the mechanism behind the 2026-08-06 orphan
// that cost $846.
//
// WHAT THIS FILE CHANGES, AND WHAT IT DELIBERATELY DOES NOT.
//
// Only the silence goes. Adoption stays lineup-only, because it structurally has
// to: EngineEvent::PosSnap is keyed on symbol_id in 1..cfg.symbols.size() and
// there is no "other" bucket to put an unrecognised symbol in. Auto-CLOSING one
// is refused for a different and stronger reason — IB's position stream carries
// no provenance (TwsIo::position ignores the account argument entirely) and this
// app supports per-symbol sub-account routing, so a rule that liquidates
// anything it does not recognise would sell the operator's own holdings, in a
// real account, unattended. Detect loudly; close by hand.
//
// The decision is a pure function of the three facts the callback has, so it
// lives here where a test can drive it (engine/tests/test_reconcile_policy.cpp)
// instead of inside a TWS callback that needs a gateway to execute.

#include <cstdint>
#include <string>

namespace tt {

enum class ReconRow {
    // A session symbol: accumulate it and adopt it at positionEnd, as before.
    Adopt,
    // Not a session symbol, and REAL: a non-zero stock position that no strategy
    // in this session watches, no stop protects, and no flatten can reach. The
    // one case that most needs saying out loud.
    OffLineupReport,
    // Not a session symbol and not evidence of anything. Two filters, the same
    // two the audit branch already justifies:
    //   - zero. IB reports a closed position as 0 for the rest of the day.
    //   - non-stock. reqPositions also reports CASH rows for a non-USD FX
    //     balance and whatever else the account holds; this app places STK
    //     orders only, so an orphan it can have CREATED is a stock. Passing
    //     these through would page Critical, unclearably, about a currency
    //     balance — and an alert that is wrong every day is how the alert that
    //     is right gets ignored.
    OffLineupIgnore,
};

// sid: the session symbol id from TwsIo::sid_for (0 = not in cfg_.symbols).
// qty: signed size from the callback. sec_type: IB's own contract.secType.
inline ReconRow classify_reconcile_row(uint32_t sid, double qty,
                                       const std::string& sec_type) {
    if (sid != 0) return ReconRow::Adopt;
    if (qty == 0.0) return ReconRow::OffLineupIgnore;
    if (sec_type != "STK") return ReconRow::OffLineupIgnore;
    return ReconRow::OffLineupReport;
}

// The operator line for an OffLineupReport row.
//
// It says what nothing will do, not merely what was found. "reconcile: position
// row NVDA 20" would have been true for 24 days and would still have read as
// routine; the fact that matters is that this session cannot close it.
inline std::string offlineup_position_line(const std::string& symbol, double qty,
                                           double avg_cost) {
    return "reconcile: OFF-LINEUP BROKER POSITION " + symbol + " " +
           std::to_string(qty) + " @ " + std::to_string(avg_cost) +
           " - this session does not trade it; NOTHING here will close it "
           "(no strategy, no stop, and flatten/kill-switch cannot reach it)";
}

// ---------------------------------------------------------------------------
// THE TWO RECONCILE DEADLINES, AND WHY THEIR ORDER IS THE SAFETY PROPERTY.
//
// 0.34.3. Two independent components each give reconciliation a deadline:
//
//   TwsBroker   gives up adopting after kBrokerReconcileTimeoutMs and clears
//               recon_active, so a stalled reqAllOpenOrders cannot disable the
//               position auditor for the session.
//   Engine      lifts its own `reconciling` gate after kEngineReconcileFailsafeMs
//               (anchored on broker->ready()), so a broker that never signals
//               completion cannot wedge trading.
//
// The engine's was 10 s and the broker's 20 s, and that ORDER is a defect. In
// the 10 s between them the engine believes it may trade while this broker has
// not yet been told which order ids the account is already using — adoption is
// still arriving. A placement drained there spends an id an adopted order is
// about to claim, and openOrder's "already ours this session" early return then
// discards the real resting order permanently: it is never adopted, so every
// fill and every cancel it reports is dropped and the app's book diverges from
// the broker's with nothing saying so. That is the 2026-08-13 phantom position,
// reached through the machinery meant to prevent it.
//
// It cannot be fixed at the placement instead. 0.34.0 tried, with a loop in
// place() skipping ids already in local_by_tws — provably dead code: openOrder
// advances next_tws_id past orderId + 1 BEFORE inserting, so every key in that
// map is always below next_tws_id and the loop can never take an iteration. In
// the seam the colliding id is precisely the one NOT yet in the map. The guard
// has to be the deadline ordering, which is the only thing that makes the
// dangerous window not exist.
//
// Nor can it be fixed by refusing placements while recon_active is true in
// order_path_ready(): that gate also covers Cancel, CancelAll and Flatten, and a
// kill switch that silently placed nothing for the first 20 s of a session is far
// worse than a collision. Safety actions must never wait on adoption.
inline constexpr int64_t kBrokerReconcileTimeoutMs = 20'000;
inline constexpr int64_t kEngineReconcileFailsafeMs = 25'000;

// The invariant, enforced at compile time. A margin, not just >: the two clocks
// are anchored at slightly different instants (the broker's at start_reconcile,
// the engine's at broker->ready()), so equality would leave the outcome to
// scheduling.
static_assert(kEngineReconcileFailsafeMs >= kBrokerReconcileTimeoutMs + 5'000,
              "the engine must not resume trading while the broker could still "
              "be adopting: a placement in that window spends an order id an "
              "adopted order is about to claim, and that order is then never "
              "adopted at all");

} // namespace tt
