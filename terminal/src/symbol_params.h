#pragma once
// Which parameter set a symbol actually trades, and whether a daily-lineup pick
// has earned a place in the session at all.
//
// WHY this exists as its own header: on 2026-08-10 all six live symbols (SNXX,
// MUU, SOXS, KORU, SOXL, SSPC) traded ONE byte-identical parameter set, fitted
// to SSPC in the 09:40:40 sweep. Two independent defects produced that:
//
//   A. Every tournament CANDIDATE sweep stamped its in-sample winner into the
//      shared per-strategy map (App::pump_sweep), before any holdout scoring and
//      with no rollback when the champion was rejected. A Trade tab with no
//      params of its own then silently picked that map up at session start, so
//      the last sweep to finish anywhere set the parameters for everyone.
//   B. SOXS, MUU, KORU and SOXL each lost all five candidates to candle-fetch
//      timeouts ("no candidate produced a result") and traded anyway — on
//      another instrument's fit, with zero validation of their own.
//
// The selection and admission rules live here, free of ImGui/Engine/panel state,
// so they can be tested directly (tests/test_symbol_params.cpp). Silent
// inheritance is what kept this invisible for weeks, so both rules report WHY
// they decided what they decided, and the callers log it.

#include "market_calendar.h"

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tt::ui {

// ------------------------------------------------- reachability of a time stop
//
// MAY THIS PARAMETER SET EVER CLOSE A LOSING POSITION TODAY?
//
// 2026-08-14, -$506.26 on STKH — 85% of the day's loss, from one trade whose
// only risk control was arithmetically incapable of firing.
//
// bollinger_reversion and rsi2_pullback deliberately place NO price stop (see
// the header of strategies/bollinger_reversion.cpp: "the time stop is the risk
// control ... `time_stop` is therefore MANDATORY — with no price stop it is the
// only thing that ever closes a losing position"). BollRev's other exit, the
// z-recovery, is gated on `bar.close > entry_px_`, so by design it cannot fire
// while the trade is underwater. On a LOSING trade the time stop is the only
// exit that exists.
//
// That morning's tournament fitted STKH's time_stop to 233 bars. Live bar size
// is 300 s, so 233 bars is 19.4 hours of bars — about three trading days. One
// RTH day is 78 five-minute bars and the engine force-flattens everything at
// 15:57 (kEodBackstopH), so the position could hold at most 77. It filled at
// 13:41 and held 27.2 bars. From the instant it filled there was nothing in the
// system that could close it at a loss: no broker stop, no z-exit, no reachable
// time stop. It fell 10.1% and the 15:57 backstop liquidated it at market.
//
// WHY the optimizer proposed an impossible number: it sweeps time_stop to 500
// bars and scores it in Engine::run, which replayed a 6-month series straight
// through with NO day boundary and no EOD flatten. A 233-bar hold is freely
// realizable in that replay (it scored +8.60%) and unreachable in production.
// The optimizer and the live engine were fitting and running under different
// physics, and the mismatch silently converted a MANDATORY risk control into
// dead code. BacktestConfig::eod_flatten_h now closes that at the source; this
// is the admission gate that refuses a fit which slipped through anyway.
//
// It is systemic, not an STKH quirk: of the five symbols live that day, TWO
// carried an unreachable time stop as their only protection (STKH 233 bars and
// SNDQ 375). SNDQ simply never entered.
//
// REFUSE, NEVER CLAMP. Clamping 233 down to 77 would have changed not one share
// of the outcome — a 13:41 entry can hold ~27 bars before 15:57 regardless — and
// it would leave the symbol trading a number no backtest ever scored. The defect
// is that the fit is unrealizable, so the answer is to sit the symbol out and
// refit it, not to invent a value.

// The tag every "this set cannot close what it opens" line carries, so
// alert_rules.h can page on the FACT rather than on a phrase someone may
// reword. Uppercase, like the lineup's own ABORTED/EXCLUDED verdicts, so
// ordinary prose about time stops cannot page anybody.
inline constexpr const char* kUnreachableStopTag = "UNREACHABLE TIME STOP";

// Bars of one FULL regular session at this bar size — from the open to the
// engine's 15:57 EOD flatten, which is the last moment a position can still be
// closed today. 77 at 300 s.
//
// Always the full-day figure, never "bars left from now": the verdict on a
// parameter set must not depend on what time the lineup happened to be built,
// or a 09:35 build and a 14:00 rebuild would disagree about the same numbers.
// INTEGER seconds of day throughout, which market_calendar.h already insists on
// for exactly this reason: 15.95 has no exact double, so (kEodBackstopH -
// kRthOpenH) * 3600 evaluates to 23219.999999999996 and floors to one bar SHORT
// at every bar size that divides the day evenly. 60 s bars came out at 386
// instead of 387 — a boundary function whose value is one under the round number
// is a trap, and here it would refuse a fit that is in fact exactly reachable.
inline int session_bars(int bar_seconds) {
    if (bar_seconds <= 0) return 0;
    const int open_sod = static_cast<int>(kRthOpenH * 3600.0 + 0.5);
    const int flat_sod = static_cast<int>(kEodBackstopH * 3600.0 + 0.5);
    return (flat_sod - open_sod) / bar_seconds;
}

// True when `params` carries no time_stop at all (the strategy does not use one,
// or it has a real price stop instead), or carries one this bar size can
// actually reach inside a single session.
//
// `disable_auto_halt` is the "hold — don't halt" preference: those symbols
// legitimately ride a position past the day, so the reachability question does
// not apply to them and asking it would refuse a configuration that is working
// as intended.
inline bool time_stop_reachable(const std::map<std::string, double>& params,
                                int bar_seconds, bool hold_dont_halt = false) {
    if (hold_dont_halt) return true;
    const auto it = params.find("time_stop");
    if (it == params.end()) return true;
    if (!(it->second > 0.0)) return true;   // 0/absent: the strategy repairs it
    const int reachable = session_bars(bar_seconds);
    if (reachable <= 0) return true;        // unknown bar size: not ours to judge
    return it->second <= static_cast<double>(reachable);
}

// ---------------------------------------------------------------- selection

// Where the values a symbol trades came from.
enum class ParamSource {
    None,        // the strategy declares no tunable parameters
    Own,         // every declared name came from THIS symbol's own set
    Mixed,       // some of its own, the rest from the strategy default
    Inherited,   // nothing of its own: the shared per-strategy default, whole
};

inline const char* param_source_name(ParamSource s) {
    switch (s) {
    case ParamSource::Own:       return "own";
    case ParamSource::Mixed:     return "mixed";
    case ParamSource::Inherited: return "inherited";
    case ParamSource::None:      break;
    }
    return "none";
}

// A parameter the strategy declares, plus the value the strategy manager holds
// for it right now — the fallback.
//
// Be precise about what that fallback IS, because the first draft of this header
// claimed more than the code can deliver: it is the strategy's SHARED value, the
// one the Strategies panel edits and the Optimizer panel's "Run"/"Tournament"
// buttons deliberately overwrite. Since 0.16.1 nothing writes it behind the
// user's back (candidate sweeps no longer do), but "shared default" still never
// means "fitted to this symbol". Inheriting it is a fact to be reported, never
// evidence that the symbol was validated.
struct ParamDefault {
    std::string name;
    double value = 0;
};

struct ParamSelection {
    std::map<std::string, double> params;   // exactly the declared names
    ParamSource source = ParamSource::None;
    int own_count = 0;
    int inherited_count = 0;
    // Declared names that fell back to the strategy default, for the log line.
    std::vector<std::string> inherited_names;
};

// Resolve the set a symbol will trade: its own value for every declared name it
// has one for, the strategy default for the rest. Undeclared keys in `own` are
// dropped — they belong to a strategy this symbol is no longer running, and
// passing them on is how a mismatched set reaches the engine unnoticed.
inline ParamSelection select_symbol_params(const std::map<std::string, double>& own,
                                           const std::vector<ParamDefault>& declared) {
    ParamSelection out;
    for (const ParamDefault& d : declared) {
        const auto it = own.find(d.name);
        if (it != own.end()) {
            out.params[d.name] = it->second;
            ++out.own_count;
        } else {
            out.params[d.name] = d.value;
            ++out.inherited_count;
            out.inherited_names.push_back(d.name);
        }
    }
    if (declared.empty())            out.source = ParamSource::None;
    else if (out.own_count == 0)     out.source = ParamSource::Inherited;
    else if (out.inherited_count == 0) out.source = ParamSource::Own;
    else                             out.source = ParamSource::Mixed;
    return out;
}

// ---------------------------------------------------------------- admission

// What this morning's lineup build achieved for one pick.
struct SymbolOutcome {
    // A tournament was attempted for this symbol in THIS lineup build. False for
    // a tab the user added by hand — admission never touches those.
    bool tournament_ran = false;
    // THIS build's tournament crowned a champion AND installed it on THIS
    // symbol's tab.
    //
    // Recorded positively, at the one line that does the install. The 0.16.0
    // draft inferred the opposite — "not in the failed list" — and only ONE
    // code path ever appended to that list (finish_tournament's champ<0 arm).
    // Every other way a tournament can end therefore read as success: the 60s
    // candle-fetch timeout, "optimizer busy", "every loaded strategy excluded",
    // the Optimizer panel's Cancel button, and the champion being rejected as
    // non-positive (which leaves the tab with nothing fitted to it at all). A
    // path nobody thought of must make this rule STRICTER, not weaker, so the
    // flag can only be set by the success itself.
    bool fitted_this_build = false;
    // The tab carries a parameter set of its own for the strategy it is
    // currently running — either this morning's champion, or a fit for THIS
    // symbol carried over from a previous session (set_lineup preserves those).
    bool has_own_params = false;
    // The account is exposed to this symbol right now: a live position, or a
    // resting order that can open one. Exclusion is a REFUSAL TO TRADE, and it
    // was being applied as "delete the tab" — which for an exposed symbol means
    // either market-closing the position (begin_lineup_swap flattens every
    // dropped symbol, bypassing hold-until-profitable) or, with no session
    // running, orphaning it: absent from cfg.symbols it is never adopted at
    // reconciliation, never appears in /diag, and is invisible to both the
    // orphan watchdog and the 15:57 EOD backstop. Neither is an improvement on
    // the borrowed-parameter bug this file exists to fix.
    bool holds_position = false;
    // The set this tab would trade can actually close a losing position today —
    // time_stop_reachable() above, against the tab's own bar size. False is the
    // 2026-08-14 STKH state: a stopless strategy whose only exit needs three
    // trading days of bars.
    //
    // Default TRUE so a caller that has not been taught this question yet cannot
    // silently exclude every symbol; the callers that HAVE been taught it
    // (App::pump_daily_lineup) always set it explicitly.
    bool time_stop_reachable = true;
};

enum class LineupAdmit {
    Admit,             // this morning's tournament spoke for it
    AdmitOwnPrevious,  // no fit today, but it still has its OWN earlier fit
    // Nothing of its own, but we are exposed. Keeping it puts it in cfg.symbols,
    // where reconciliation adopts the position and adopt_hold pauses its
    // strategy until the position goes flat — so for as long as the exposure
    // lasts, the symbol is managed and cannot enter on borrowed parameters.
    // KNOWN RESIDUAL: once it IS flat, adopt_hold lifts and the strategy resumes
    // on inherited values like any adopted symbol. Closing that needs a
    // per-symbol "no new entries" gate in the engine, which does not exist yet;
    // the alternative — dropping the tab — is strictly worse, because it either
    // market-closes the position or orphans it entirely.
    AdmitHoldingOnly,
    Exclude,           // no fit, nothing of its own, no exposure — sit it out
    // Its only exit cannot fire inside a trading day (see time_stop_reachable).
    // A SEPARATE verdict from Exclude because the two say opposite things about
    // the fit: Exclude means "nothing was fitted to this symbol", this means
    // "something was, and it is unrealizable" — a defect in the fit, which the
    // operator has to hear as its own sentence rather than folded into the
    // ordinary no-fit line.
    ExcludeUnreachableStop,
};

// The rule: a symbol may trade only on parameters fitted to ITSELF, and only on
// parameters that can close a losing position before the day ends. This
// morning's champion is best; its own earlier fit is stale but still fitted to
// this instrument; another instrument's fit is never acceptable, so a symbol
// left with nothing of its own sits the session out — unless we are already in
// it, in which case it stays so the position can be managed to flat.
inline LineupAdmit admit_lineup_symbol(const SymbolOutcome& o) {
    // FIRST, and ahead of the hand-added bypass: this is a risk-control test,
    // not a fit-quality one. "Not ours to judge" is the right answer to "did a
    // tournament validate this symbol"; it is not the right answer to "can
    // anything in this system close the position it is about to open".
    //
    // The exposure carve-out is the same one AdmitHoldingOnly rests on, and for
    // the same reason: dropping an exposed symbol either market-closes the
    // position (begin_lineup_swap flattens every drop) or orphans it outside
    // cfg.symbols, where reconciliation cannot adopt it and the 15:57 backstop
    // cannot flatten it — see engine/reconcile_policy.h for what that costs.
    // KNOWN RESIDUAL, stated rather than hidden: a symbol kept for that reason
    // can enter again on the unreachable set once it goes flat. Closing that
    // needs a per-symbol no-new-entries gate in the engine, which does not
    // exist; orphaning a live position is strictly worse than the residual.
    if (!o.time_stop_reachable && !o.holds_position)
        return LineupAdmit::ExcludeUnreachableStop;
    if (!o.tournament_ran) return LineupAdmit::Admit;   // hand-added tab: not ours to judge
    if (o.fitted_this_build) return LineupAdmit::Admit;
    if (o.has_own_params) return LineupAdmit::AdmitOwnPrevious;
    return o.holds_position ? LineupAdmit::AdmitHoldingOnly : LineupAdmit::Exclude;
}

struct LineupPlan {
    std::vector<std::string> admitted;       // symbols the session will trade
    std::vector<std::string> own_previous;   // subset of admitted, on a stale own fit
    std::vector<std::string> holding_only;   // subset of admitted, kept only to go flat
    std::vector<std::string> excluded;       // dropped: no validated set of their own
    // Subset of `excluded`: dropped because their only exit cannot fire today.
    // Kept in `excluded` as well so every existing caller (and every existing
    // test) still sees the full set of symbols that will not trade; this list
    // exists only so the operator line can say WHY, which is a different fault
    // from "nothing was fitted to it".
    std::vector<std::string> unreachable_stop;
    // False = start NOTHING. Every pick failed, and a session of borrowed
    // parameters is worse than no session; the caller logs loudly instead.
    bool start = false;
};

inline LineupPlan plan_lineup(
    const std::vector<std::pair<std::string, SymbolOutcome>>& outcomes) {
    LineupPlan p;
    for (const auto& [sym, o] : outcomes) {
        switch (admit_lineup_symbol(o)) {
        case LineupAdmit::Admit:
            p.admitted.push_back(sym);
            break;
        case LineupAdmit::AdmitOwnPrevious:
            p.admitted.push_back(sym);
            p.own_previous.push_back(sym);
            break;
        case LineupAdmit::AdmitHoldingOnly:
            p.admitted.push_back(sym);
            p.holding_only.push_back(sym);
            break;
        case LineupAdmit::Exclude:
            p.excluded.push_back(sym);
            break;
        case LineupAdmit::ExcludeUnreachableStop:
            p.excluded.push_back(sym);
            p.unreachable_stop.push_back(sym);
            break;
        }
    }
    p.start = !p.admitted.empty();
    return p;
}

} // namespace tt::ui
