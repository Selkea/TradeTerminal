#pragma once

// Does the app's book still match the BROKER's book?
//
// WHY THIS EXISTS. On 2026-08-13 a lineup swap restarted the live session on a
// new TWS orders client id. The new client adopted RAM's resting take-profit out
// of a reqAllOpenOrders snapshot, but IB scopes orderStatus / execDetails /
// commissionReport to the client that PLACED an order — so when the limit filled,
// the app was never told. It carried a PHANTOM 411-share position for 4 h 03 m,
// published a fictional +$528 unrealized and reported the symbol "protected". A
// manual sell in that state would have opened a naked short. Nothing anywhere
// noticed, because there is no callback for a callback that does not arrive.
//
// tws_client_id.h fixes the CAUSE (the orders id no longer rotates). This file
// is the layer that has to work anyway. The detection gap is wider than the one
// bug: a dropped callback, an order placed by hand in the Gateway GUI, a partial
// fill, a future refactor — every one of them re-opens it, and the app's book is
// derived state that has no way to audit itself. The only authority on what this
// account actually holds is the account.
//
// TWO RULES, both non-negotiable, both learned here:
//
//   1. IT DETECTS, IT DOES NOT FIX. reqPositions could re-seed the portfolio and
//      make the number "right" — and would erase the evidence that the order
//      path is deaf while leaving the deafness in place. A divergence means one
//      of the two sides is lying and code cannot tell which; the only correct
//      automatic action is to say so, loudly, and stop.
//
//   2. IT MUST BE ABLE TO SEE ITS OWN FAILURE. This project's recurring defect
//      is instrumentation that reads healthy while broken:
//      oldest_history_age_ms pinned at 0 through a five-hour outage (it was keyed
//      off a pending set that dead requests were removed from), data.connected
//      true against a login modal, stuck_orders sitting at 2 with nothing
//      watching. So the state below is NOT a divergence count — a count is 0 both
//      when the books agree and when no audit has been answered since 09:31.
//      An auditor that stops getting answers reports Blind and an age that
//      GROWS, and it can never report Ok without a fresh answer that agreed.
//
// The values the /diag `book_divergence` field takes, exhaustively:
//
//      "off"                          no live session / no reconciling broker
//      "starting"                      armed, first answer not back yet
//      "ok"                            last answer agreed, and it is recent
//      "suspect: RAM app=411 broker=0 (1/2)"    seen once, not yet confirmed
//      "DIVERGED: RAM app=411 broker=0"         confirmed; this pages
//      "unknown: no broker answer for 312s"     the auditor itself is blind
//
// Healthy is "ok". Broken is never "ok": a phantom reads DIVERGED, and an
// auditor that has stopped being answered reads "unknown" with a rising age.
// The one number that is always meaningful is last_agreed_age_ms — ms since an
// audit last came back AGREEING — which climbs in every failure mode there is,
// including the ones nobody has thought of.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tt::net {

// The uppercase paging tags (see terminal/src/alert_rules.h). Uppercase for the
// same reason the lineup's verdicts are: ordinary prose about positions must not
// page anyone.
inline constexpr const char* kBookDivergenceTag = "BOOK DIVERGENCE";
// The auditor is armed and not being answered — the detector is off, which is a
// separate and equally pageable fact from the books disagreeing.
inline constexpr const char* kBookAuditBlindTag = "BOOK AUDIT BLIND";

// One symbol's signed quantity on one side of the comparison (negative = short).
struct BookPos {
    std::string symbol;
    double qty = 0.0;
};

// Share counts are whole numbers on this route, but they arrive as doubles
// through IB's Decimal and through the engine's portfolio, so the comparison
// needs a tolerance. Far below one share: a one-share disagreement is a real
// disagreement and must not be rounded away.
inline constexpr double kQtyEpsilon = 1e-6;

enum class DivergeKind {
    Quantity,     // both sides hold it, different sizes
    Sign,         // both sides hold it, OPPOSITE directions
    AppOnly,      // the app holds it, the broker reports nothing (the phantom)
    BrokerOnly,   // the broker holds it, the app thinks it is flat (the orphan)
};

inline const char* diverge_kind_name(DivergeKind k) {
    switch (k) {
    case DivergeKind::Quantity:   return "quantity";
    case DivergeKind::Sign:       return "sign";
    case DivergeKind::AppOnly:    return "app-only";
    case DivergeKind::BrokerOnly: return "broker-only";
    }
    return "?";
}

struct Divergence {
    std::string symbol;
    double app_qty = 0.0;
    double broker_qty = 0.0;
    DivergeKind kind = DivergeKind::Quantity;
    // Consecutive audits this EXACT disagreement (same symbol, same pair of
    // quantities) has survived. 1 = seen once. See BookAudit for why that is not
    // enough to page on.
    int rounds = 0;
};

// Compare two books. Pure: no clock, no broker, no engine.
//
// `settling` names symbols whose app-side quantity CHANGED while the broker's
// snapshot was in flight — an ordinary fill landing mid-audit. Those are skipped
// entirely rather than reported and then explained away, because a comparison of
// two books taken at different instants is not evidence of anything. See
// BookAudit::observe for the other half of the race handling.
//
// Symbols reading zero on both sides are not "in agreement about nothing" —
// they are simply absent from the result, which is the same thing.
inline std::vector<Divergence> compare_books(const std::vector<BookPos>& app,
                                             const std::vector<BookPos>& broker,
                                             const std::vector<std::string>& settling = {}) {
    auto is_settling = [&](const std::string& s) {
        for (const std::string& x : settling)
            if (x == s) return true;
        return false;
    };
    std::unordered_map<std::string, double> b;
    for (const BookPos& p : broker) b[p.symbol] = p.qty;

    std::vector<Divergence> out;
    for (const BookPos& p : app) {
        if (is_settling(p.symbol)) continue;
        const auto it = b.find(p.symbol);
        if (it == b.end()) {
            // The broker did not mention it at all. That is a statement — IB
            // reports every position the account holds — so it means flat.
            if (std::fabs(p.qty) > kQtyEpsilon)
                out.push_back({p.symbol, p.qty, 0.0, DivergeKind::AppOnly, 0});
            continue;
        }
        const double bq = it->second;
        if (std::fabs(p.qty - bq) <= kQtyEpsilon) continue;   // agree
        // Sign first, though a flip is also a quantity mismatch: "long 400 where
        // the broker is short 400" is a different morning from "long 400 vs 411",
        // and the label is what the operator reads at 03:00. Both sides must be
        // genuinely non-flat for a direction to exist at all.
        const bool both_live = std::fabs(p.qty) > kQtyEpsilon &&
                               std::fabs(bq) > kQtyEpsilon;
        const DivergeKind kind =
            both_live && ((p.qty > 0) != (bq > 0)) ? DivergeKind::Sign
                                                   : DivergeKind::Quantity;
        out.push_back({p.symbol, p.qty, bq, kind, 0});
    }
    // A position the broker holds in a symbol the app's book does not list at
    // all. On the live path the book always lists every session symbol, so this
    // is the reconciliation-never-ran / symbol-dropped case — and it is the
    // dangerous direction: a REAL position with no strategy and no stop.
    for (const BookPos& p : broker) {
        if (is_settling(p.symbol)) continue;
        if (std::fabs(p.qty) <= kQtyEpsilon) continue;
        bool in_app = false;
        for (const BookPos& a : app)
            if (a.symbol == p.symbol) { in_app = true; break; }
        if (!in_app) out.push_back({p.symbol, 0.0, p.qty, DivergeKind::BrokerOnly, 0});
    }
    // Stable order so the /diag string and the page text don't shuffle between
    // rounds and read as new events. Tiny n (a lineup is <10).
    for (size_t i = 1; i < out.size(); ++i)
        for (size_t j = i; j > 0 && out[j].symbol < out[j - 1].symbol; --j)
            std::swap(out[j], out[j - 1]);
    return out;
}

// How many symbols a round actually LOOKED AT — i.e. how much evidence an empty
// compare_books() result carries.
//
// An empty result means one of two completely different things: "every symbol
// agreed" or "there was nothing to compare". Recording them the same way is how
// the auditor came to report Ok, and reset the age a remote alert rule watches,
// on a round in which every symbol had been excluded as `settling` — a
// one-symbol lineup with a fill in flight does exactly that. The file's own
// contract is that it "can never report Ok without a fresh answer that agreed",
// and a comparison of nothing is not that answer.
inline size_t comparable_count(const std::vector<BookPos>& app,
                               const std::vector<BookPos>& broker,
                               const std::vector<std::string>& settling = {}) {
    auto is_settling = [&](const std::string& s) {
        for (const std::string& x : settling)
            if (x == s) return true;
        return false;
    };
    size_t n = 0;
    for (const BookPos& p : app)
        if (!is_settling(p.symbol)) ++n;
    for (const BookPos& p : broker) {
        if (is_settling(p.symbol)) continue;
        bool in_app = false;
        for (const BookPos& a : app)
            if (a.symbol == p.symbol) { in_app = true; break; }
        if (!in_app) ++n;   // broker-only: comparable, and the dangerous one
    }
    return n;
}

// ---- cadence ---------------------------------------------------------------

// How often the app asks the broker what it holds.
//
// 60 s. The requirement is minutes, not hours — the 2026-08-13 phantom lived
// 4 h 03 m — and the cost is provably negligible: reqPositions is an
// account-level subscription, two messages plus one per position, and IB's
// pacing limit is 50 messages per second. It is NOT under the historical-data
// pacing rules that the tournament fights with (hist_pacing.h), so this cannot
// starve the bar fetches the optimizer depends on. Faster buys nothing: a
// divergence that matters persists, and one that does not is a fill in flight.
inline constexpr int64_t kBookAuditIntervalMs = 60'000;

// How many CONSECUTIVE audits must show the same disagreement before it pages.
//
// This is the second half of the in-flight-fill race handling, and the reason it
// is 2 rather than 1. The dangerous transient looks identical to the bug: the
// broker has filled an exit and reported it, but the engine has not drained the
// event yet, so the broker reads 0 while the book still reads 411. Excluding
// symbols that moved DURING the audit (compare_books' `settling`) cannot catch
// that one, because the app-side quantity has not moved yet — it is about to.
//
// What separates them is time. The engine drains its event ring every frame
// (~16 ms) and the fill is applied the moment execDetails arrives, so a genuine
// in-flight fill cannot still be outstanding 60 seconds later. A callback that
// is never coming can. Requiring the SAME pair of quantities twice, a minute
// apart, therefore distinguishes them on the only axis that actually differs —
// at a cost of one minute of detection latency against four hours of blindness.
inline constexpr int kBookAuditConfirmRounds = 2;

// How long the auditor may go unanswered before it declares itself blind.
//
// Three missed answers. Long enough that one dropped snapshot around a
// reconnect is not an event; short enough that the detector's own death is
// visible in the same "minutes, not hours" the detector promises. This pages,
// separately from a divergence: an auditor nobody is answering is not a healthy
// book, it is no information at all, and the whole point of this file is that
// those two must never look alike.
inline constexpr int64_t kBookAuditBlindMs = 3 * kBookAuditIntervalMs;

enum class AuditState {
    Off,        // not armed: no live session, or no broker that can be asked
    Starting,   // armed, nothing answered yet this session
    Ok,         // the last answer agreed and is recent
    Suspect,    // a disagreement seen, not yet confirmed
    Diverged,   // confirmed: pages
    Blind,      // armed and unanswered past kBookAuditBlindMs
};

inline const char* audit_state_name(AuditState s) {
    switch (s) {
    case AuditState::Off:      return "off";
    case AuditState::Starting: return "starting";
    case AuditState::Ok:       return "ok";
    case AuditState::Suspect:  return "suspect";
    case AuditState::Diverged: return "DIVERGED";
    case AuditState::Blind:    return "unknown";
    }
    return "?";
}

// "RAM app=411 broker=0 (app-only)"
inline std::string describe_divergence(const Divergence& d) {
    auto num = [](double v) {
        // Whole shares read as whole shares; std::to_string(411.0) is
        // "411.000000" and this string ends up in a phone alert.
        char buf[48];
        if (std::fabs(v - std::llround(v)) < kQtyEpsilon)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
        else
            std::snprintf(buf, sizeof buf, "%.4f", v);
        return std::string(buf);
    };
    return d.symbol + " app=" + num(d.app_qty) + " broker=" + num(d.broker_qty) +
           " (" + diverge_kind_name(d.kind) + ")";
}

inline std::string describe_divergences(const std::vector<Divergence>& ds) {
    std::string s;
    for (const Divergence& d : ds) {
        if (!s.empty()) s += "; ";
        s += describe_divergence(d);
    }
    return s;
}

// The page. Names the symbol, the app's quantity and the broker's — the three
// facts the operator needs before touching anything.
//
// It says what to check rather than what to do, on purpose. Both directions are
// possible and they call for opposite actions: an app-only divergence is a
// phantom, where selling would open a naked short, and a broker-only one is a
// real position with nobody managing it, where doing nothing is the loss. The
// alert cannot tell which without a human looking at the Gateway.
inline std::string book_divergence_alert(const std::vector<Divergence>& ds,
                                         int64_t confirmed_over_ms) {
    return std::string(kBookDivergenceTag) +
           " the broker and the app disagree about a LIVE position, unchanged "
           "over " + std::to_string(confirmed_over_ms / 1000) + "s: " +
           describe_divergences(ds) +
           " - the app's number is NOT the account's. Check the position in IB "
           "Gateway before trading this symbol: an app-only divergence means the "
           "book is holding a position that closed (selling it would open a NAKED "
           "SHORT), a broker-only one means a real position nothing is watching.";
}

inline std::string book_audit_blind_alert(int64_t age_ms) {
    return std::string(kBookAuditBlindTag) +
           " the broker has not answered a position request for " +
           std::to_string(age_ms / 1000) +
           "s during a live session, so the app can no longer tell whether its "
           "book matches the account. This is not 'no divergence' - it is no "
           "information: the check that would catch a phantom position is off. "
           "The order socket still reports connected, so check the tws log for a "
           "half-open API session.";
}

// ---- the auditor -----------------------------------------------------------
//
// Owns the streak bookkeeping and the freshness clock; the App owns the wire.
// Pure arithmetic on a caller-supplied steady-clock `now_ms`, for the same
// reason WatchdogTimer is (see watchdog_timer.h): the 2026-08-11 minimized
// window stopped ImGui's clock dead and froze every watchdog that read it.
class BookAudit {
public:
    // The auditor no longer applies at all (session stopped, broker gone). Not
    // the same as "the books agree" — it clears the streaks and the clock so a
    // new session starts from Starting rather than inheriting yesterday's Ok.
    void stand_down() {
        armed_ = false;
        state_ = AuditState::Off;
        streaks_.clear();
        confirmed_.clear();
        // The live book belongs to a SESSION — a dead session's book proves
        // nothing about the current one, so the auditor's own state is cleared.
        // But it is KEPT here first, because the one caller that runs BETWEEN
        // sessions has nothing else to ask.
        //
        // start_live_session refuses a symbol whose time stop cannot fire, and
        // carves out "unless the broker might be holding it" so the refusal
        // cannot orphan a real position. That gate runs while no session is
        // live, i.e. after stand_down() — so reading the cleared live book made
        // `unknown` unconditionally true and the gate could NEVER refuse
        // anything. A guard whose default is "unproven, so allow" is no guard;
        // it is exactly the shape of the 2026-08-10 defect.
        if (!last_broker_.empty()) last_stopped_broker_ = last_broker_;
        last_broker_.clear();
        last_agreed_ms_ = 0;
        last_answer_ms_ = 0;
        armed_at_ms_ = 0;
        confirmed_span_ms_ = 0;
        rounds_ = 0;
    }

    // Armed but nothing answered yet — call this on the frame the session comes
    // up, so the blind clock has an origin that is not the epoch.
    void arm(int64_t now_ms) {
        if (armed_) return;
        armed_ = true;
        armed_at_ms_ = now_ms;
        state_ = AuditState::Starting;
    }

    // One completed audit. `app` and `broker` are the two books; `settling` is
    // the symbols whose app-side quantity moved while the broker's answer was in
    // flight (excluded — see compare_books).
    //
    // Returns the CONFIRMED divergences, i.e. those that have now survived
    // kBookAuditConfirmRounds consecutive audits unchanged. Empty on the first
    // sighting, which is the point.
    std::vector<Divergence> observe(const std::vector<BookPos>& app,
                                    const std::vector<BookPos>& broker,
                                    const std::vector<std::string>& settling,
                                    int64_t now_ms) {
        if (!armed_) arm(now_ms);
        ++rounds_;
        last_answer_ms_ = now_ms;   // an answer DID arrive: this is not blindness
        // Kept whole, before any of the comparison logic can discard rows as
        // settling or uncomparable. This is the ONLY place in the app that holds
        // the broker's actual book — reconciliation cannot, by construction (see
        // engine/reconcile_policy.h: it drops every symbol outside the lineup) —
        // and App::begin_lineup_swap needs it to answer "is this symbol I am
        // about to drop really flat AT THE BROKER". Answering that from the
        // app's own snapshot is what let the 2026-08-06 orphan out: the app's
        // number is the number this whole class of bug corrupts.
        last_broker_ = broker;
        // A round that compared nothing is not agreement. Everything below —
        // the streaks, the confirmed set, and above all last_agreed_ms_, which
        // is the number the remote alert rule watches — is a claim about
        // evidence, and there is none here. Leave all of it exactly as the last
        // round that did have some left it; the age simply keeps climbing,
        // which is the correct description of what just happened.
        if (comparable_count(app, broker, settling) == 0) return confirmed_;
        std::vector<Divergence> seen = compare_books(app, broker, settling);

        // Advance the streak only for a disagreement that is IDENTICAL to last
        // round's. A pair of quantities that is still moving is a book in
        // motion, not a book that has diverged — so a partial fill working
        // through in stages resets rather than accumulates.
        std::unordered_map<std::string, Entry> next;
        for (Divergence& d : seen) {
            const auto it = streaks_.find(d.symbol);
            const bool same = it != streaks_.end() &&
                              std::fabs(it->second.app_qty - d.app_qty) <= kQtyEpsilon &&
                              std::fabs(it->second.broker_qty - d.broker_qty) <= kQtyEpsilon;
            const int64_t since = same ? it->second.since_ms : now_ms;
            d.rounds = same ? it->second.rounds + 1 : 1;
            next[d.symbol] = Entry{d.app_qty, d.broker_qty, d.kind, d.rounds, since};
        }
        streaks_.swap(next);
        // The window the confirmation actually covers, quoted in the page: the
        // oldest surviving streak. "Unchanged over 60s" is the claim that makes
        // the alert trustworthy, so it must be measured, not assumed from the
        // cadence — a frame that skipped an audit would otherwise overstate it.
        confirmed_span_ms_ = 0;
        for (const auto& [sym, e] : streaks_) {
            (void)sym;
            if (e.rounds < kBookAuditConfirmRounds) continue;
            const int64_t span = now_ms - e.since_ms;
            if (span > confirmed_span_ms_) confirmed_span_ms_ = span;
        }

        confirmed_.clear();
        for (const Divergence& d : seen)
            if (d.rounds >= kBookAuditConfirmRounds) confirmed_.push_back(d);

        if (seen.empty()) {
            last_agreed_ms_ = now_ms;
            state_ = AuditState::Ok;
        } else {
            state_ = confirmed_.empty() ? AuditState::Suspect : AuditState::Diverged;
        }
        return confirmed_;
    }

    // Called every frame with the current time, whether or not an answer came
    // back. This is what makes the metric able to see its own failure: with no
    // answers the state degrades to Blind on its own, rather than sitting on the
    // last Ok it happened to record.
    AuditState tick(int64_t now_ms) {
        if (!armed_) return AuditState::Off;
        if (unanswered_ms(now_ms) > kBookAuditBlindMs) state_ = AuditState::Blind;
        return state_;
    }

    AuditState state() const { return state_; }
    bool blind() const { return state_ == AuditState::Blind; }

    // The broker's book as of the most recent completed audit — every row it
    // sent, including symbols this session does not trade. Empty before the
    // first answer and after stand_down().
    const std::vector<BookPos>& last_broker() const { return last_broker_; }

    // Is the BROKER carrying a position in `symbol` right now, as far as the
    // last audit could tell?
    //
    // `unknown` is the answer when no audit has ever come back, and it is NOT
    // folded into false. The caller (a lineup swap deciding whether it may drop
    // a symbol) has to distinguish "the broker says flat" from "nobody has
    // asked" — treating silence as flat is precisely the assumption that
    // orphaned a live position on 2026-08-06.
    bool broker_holds(const std::string& symbol, bool& unknown) const {
        unknown = last_broker_.empty();
        for (const BookPos& p : last_broker_)
            if (p.symbol == symbol) return std::fabs(p.qty) > kQtyEpsilon;
        return false;
    }

    // The same question asked BETWEEN sessions, where last_broker_ has been
    // cleared by stand_down() and broker_holds() can therefore only ever answer
    // "unknown". Falls back to the last book the previous session saw.
    //
    // `unknown` is still not folded into false: if no session has ever had an
    // answer there is genuinely nothing to go on, and the caller must treat that
    // as "may be holding" rather than as "flat" (2026-08-06, $846 orphaned by
    // exactly that conflation). What this fixes is the OTHER direction — a
    // previous session that did get a clean answer is now allowed to inform the
    // decision instead of being thrown away and read as silence.
    //
    // Deliberately a SEPARATE accessor. broker_holds() must keep meaning "as of
    // the live audit" for the divergence pages, which must never be raised off a
    // stale book.
    bool broker_held_last_session(const std::string& symbol, bool& unknown) const {
        if (!last_broker_.empty()) return broker_holds(symbol, unknown);
        unknown = last_stopped_broker_.empty();
        for (const BookPos& p : last_stopped_broker_)
            if (p.symbol == symbol) return std::fabs(p.qty) > kQtyEpsilon;
        return false;
    }
    const std::vector<Divergence>& confirmed() const { return confirmed_; }
    // How long the confirmed disagreement has held the SAME pair of quantities.
    // The page quotes it, because "unchanged over 61s" is the fact that rules
    // out an in-flight fill and "we saw it twice" is not.
    int64_t confirmed_span_ms() const { return confirmed_span_ms_; }
    int rounds() const { return rounds_; }

    // ms since the last audit that came back AND agreed. The single number that
    // is meaningful in every state: it climbs when the books disagree, it climbs
    // when nothing answers, and it is only ever reset by positive evidence.
    //
    // WITH NO AGREEMENT YET THIS SESSION IT COUNTS FROM ARMING, not -1. Reading
    // -1 there looked like the honest answer ("we have not measured one") and
    // was the wrong one: -1 is below every `>` threshold a remote rule can
    // write, so in the one failure mode this gauge exists for — armed and never
    // answered, e.g. a reconcile that wedged start_audit for the session — the
    // metric read BETTER than healthy all day while the detector was dead, next
    // to a divergence count also pinned at 0. That is the exact shape of
    // oldest_history_age_ms sitting at 0 through a five-hour outage, which this
    // file's header names as the mistake not to repeat.
    //
    // "No agreeing audit for 1800 s" is a fact the auditor HAS measured, from an
    // origin it recorded itself (arm()). The only genuinely unmeasurable case is
    // that there is no live session at all, and that is the -1 that remains — so
    // -1 also still distinguishes a Saturday scrape from a broken weekday one.
    int64_t last_agreed_age_ms(int64_t now_ms) const {
        if (!armed_) return -1;   // no session: nothing to be stale about
        return now_ms - (last_agreed_ms_ ? last_agreed_ms_ : armed_at_ms_);
    }

    // ms since ANY answer (agreeing or not); from arming if none has arrived.
    int64_t unanswered_ms(int64_t now_ms) const {
        if (!armed_) return 0;
        return now_ms - (last_answer_ms_ ? last_answer_ms_ : armed_at_ms_);
    }

    // The /diag string. One field an operator can read at a glance, whose
    // healthy value ("ok") is unreachable from every broken state.
    std::string field(int64_t now_ms) const {
        switch (state_) {
        case AuditState::Off:
            return "off";
        case AuditState::Starting:
            return "starting";
        case AuditState::Ok:
            return "ok";
        case AuditState::Suspect: {
            // Sorted, like compare_books' output: a field that reshuffles between
            // rounds reads as a new event to whoever is diffing /diag.
            std::vector<Divergence> ds;
            ds.reserve(streaks_.size());
            for (const auto& [sym, e] : streaks_)
                ds.push_back({sym, e.app_qty, e.broker_qty, e.kind, e.rounds});
            for (size_t i = 1; i < ds.size(); ++i)
                for (size_t j = i; j > 0 && ds[j].symbol < ds[j - 1].symbol; --j)
                    std::swap(ds[j], ds[j - 1]);
            std::string s = "suspect: ";
            bool first = true;
            for (const Divergence& d : ds) {
                if (!first) s += "; ";
                first = false;
                s += describe_divergence(d) + " (" + std::to_string(d.rounds) + "/" +
                     std::to_string(kBookAuditConfirmRounds) + ")";
            }
            return s;
        }
        case AuditState::Diverged:
            return std::string(audit_state_name(state_)) + ": " +
                   describe_divergences(confirmed_);
        case AuditState::Blind:
            return "unknown: no broker answer for " +
                   std::to_string(unanswered_ms(now_ms) / 1000) + "s";
        }
        return "?";
    }

private:
    struct Entry {
        double app_qty = 0.0;
        double broker_qty = 0.0;
        DivergeKind kind = DivergeKind::Quantity;
        int rounds = 0;
        int64_t since_ms = 0;
    };


    bool armed_ = false;
    AuditState state_ = AuditState::Off;
    std::unordered_map<std::string, Entry> streaks_;
    std::vector<Divergence> confirmed_;
    std::vector<BookPos> last_broker_;   // the last answer, whole (see last_broker())
    // The last non-empty book, preserved across stand_down() for the one caller
    // that runs between sessions. Never used for paging — see
    // broker_held_last_session().
    std::vector<BookPos> last_stopped_broker_;
    int64_t last_agreed_ms_ = 0;    // 0 = never
    int64_t last_answer_ms_ = 0;    // 0 = never
    int64_t armed_at_ms_ = 0;
    int64_t confirmed_span_ms_ = 0;   // how long the confirmed set has held still
    int rounds_ = 0;
};

// ---- what the OPERATOR was told ---------------------------------------------
//
// The alert's memory, deliberately separate from the auditor's state, because
// the two answer different questions. AuditState is "what did the last round
// show"; this is "is the operator still owed an answer".
//
// A confirmed divergence is latched here and cleared ONLY by an audit that comes
// back agreeing. Driving the page's watchdog straight off `state == Diverged`
// looked equivalent and was not: WatchdogTimer reports Recovered on any
// transition to not-bad, and Diverged has two exits that are not recoveries.
//   - SUSPECT. A divergence whose quantities MOVE — a resting exit the app is
//     deaf to, filling in stages — resets the streak and empties the confirmed
//     set. That alternated PAGE / "the books agree again" / PAGE every 60 s
//     while /diag read "suspect: RAM app=411 broker=100 (1/2)".
//   - BLIND. Answers simply stop after the page. The all-clear fired on a
//     divergence nobody could see any more — and if the order socket was what
//     went down, the compensating BOOK AUDIT BLIND page is suppressed, so
//     "the app's book and the broker's agree again" was the operator's LAST
//     word about a live phantom position.
// Neither state carries evidence in either direction, so neither moves this.
class DivergenceLatch {
public:
    // One round of the auditor, as reported by BookAudit.
    void update(AuditState state, const std::vector<Divergence>& confirmed,
                int64_t confirmed_span_ms) {
        if (state == AuditState::Diverged) {
            open_ = confirmed;
            span_ms_ = confirmed_span_ms;
        } else if (state == AuditState::Ok) {
            clear();   // the only positive evidence there is
        }
    }

    // The session ended. Not the same as "it cleared" — nothing is announced.
    void clear() {
        open_.clear();
        span_ms_ = 0;
    }

    bool open() const { return !open_.empty(); }
    // The set to NAME in the page. Held across Suspect/Blind on purpose: a
    // 15-minute re-page that read BookAudit::confirmed() while the state had
    // slipped would name no symbols at all.
    const std::vector<Divergence>& divergences() const { return open_; }
    int64_t span_ms() const { return span_ms_; }

private:
    std::vector<Divergence> open_;
    int64_t span_ms_ = 0;
};

} // namespace tt::net
