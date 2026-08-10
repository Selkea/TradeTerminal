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

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tt::ui {

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
// for it right now (the fallback — a strategy-level default, never another
// symbol's fit once App stops writing candidate sweeps into that map).
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

// What the morning's tournament produced for one lineup pick.
struct SymbolOutcome {
    // A tournament was attempted for this symbol in THIS lineup build. False for
    // a tab the user added by hand — admission never touches those.
    bool tournament_ran = false;
    // At least one candidate came back with a score (champ >= 0). False is the
    // 2026-08-10 case: every candidate lost to a candle-fetch timeout.
    bool produced_result = false;
    // The tab carries a non-empty parameter map of its own — either this
    // morning's champion, or a fit for THIS symbol carried over from a previous
    // session (set_lineup preserves those).
    bool has_own_params = false;
};

enum class LineupAdmit {
    Admit,             // this morning's tournament spoke for it
    AdmitOwnPrevious,  // no result today, but it still has its OWN earlier fit
    Exclude,           // no result and nothing of its own — do not trade it
};

// The rule: a symbol may trade only on parameters fitted to ITSELF. Today's
// champion is best; its own earlier fit is stale but still fitted to this
// instrument; another instrument's fit is never acceptable, so a symbol left
// with nothing of its own sits the session out.
inline LineupAdmit admit_lineup_symbol(const SymbolOutcome& o) {
    if (!o.tournament_ran) return LineupAdmit::Admit;   // hand-added tab: not ours to judge
    if (o.produced_result) return LineupAdmit::Admit;
    return o.has_own_params ? LineupAdmit::AdmitOwnPrevious : LineupAdmit::Exclude;
}

struct LineupPlan {
    std::vector<std::string> admitted;       // symbols the session will trade
    std::vector<std::string> own_previous;   // subset of admitted, on a stale own fit
    std::vector<std::string> excluded;       // dropped: no validated set of their own
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
        case LineupAdmit::Exclude:
            p.excluded.push_back(sym);
            break;
        }
    }
    p.start = !p.admitted.empty();
    return p;
}

} // namespace tt::ui
