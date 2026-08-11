#pragma once
// What a headless daily-lineup dry run (TT_AUTORUN_LINEUP=1) reports, and how.
//
// WHY THIS EXISTS AS ITS OWN HEADER. The daily lineup is the one production path
// that cannot be exercised without a data session, a market scanner and roughly
// five minutes of optimizer time, so until now the only way to find out what a
// build did was to read a morning's worth of prose in optimizer.log and rebuild
// the timeline by hand. That is how the 2026-08-10 profile was produced (1543 s,
// 97.3% dead IO wait) — after the fact, from delivery timestamps, because
// nothing in the log stated a duration or a count.
//
// So the dry run emits NUMBERS, on their own greppable lines, and the format
// lives here — free of ImGui, Engine and App state — so tests can pin it
// (tests/test_lineup_dryrun.cpp). A report format that only exists inside a
// 4000-line draw() cannot be verified without the whole app and a live gateway,
// which is precisely the thing the dry run is trying to stop being true.
//
// The dry run measures the REAL path: App triggers start_daily_lineup() and then
// only observes it. Nothing here decides anything about the build — it formats
// what the build did, plus one verdict (dryrun_exit_code) so a script can branch
// on it. If this file ever starts driving the lineup instead of describing it,
// the run has stopped being evidence about production.

#include <cstdint>
#include <string>
#include <vector>

namespace tt::ui {

// THE LINE CONTRACT, and it is worth stating exactly because a report nobody can
// parse is a report nobody reads:
//
//     dryrun: kind=<what> key=value key=value ...
//
// Field 0 is this tag and nothing else in the app emits it, so `grep '^dryrun:'`
// extracts the report from a log full of prose. EVERY remaining field is a
// key=value pair with no spaces on either side — including the `kind`, so a
// reader never has to special-case a bare marker word. Keep both properties:
// they are what let `awk -F' ' '{...}'` work at all.
inline constexpr const char* kDryRunTag = "dryrun:";

// What this morning's build achieved for one pick. The first three are the
// admission verdicts plan_lineup() reaches (symbol_params.h); the last two are
// the ways a pick can leave the build with nothing fitted to itself.
enum class DryRunOutcome {
    Fitted,        // this build's tournament crowned AND installed a champion
    OwnPrevious,   // no fit today, but it still carries its own earlier fit
    HoldingOnly,   // nothing of its own; kept only so an open position is managed
    NoCandidate,   // tournament produced no usable result (or never ran)
    Excluded,      // no fit, nothing of its own, no exposure — sat out
};

inline const char* dryrun_outcome_name(DryRunOutcome o) {
    switch (o) {
    case DryRunOutcome::Fitted:       return "fitted";
    case DryRunOutcome::OwnPrevious:  return "own-previous";
    case DryRunOutcome::HoldingOnly:  return "holding-only";
    case DryRunOutcome::Excluded:     return "excluded";
    case DryRunOutcome::NoCandidate:  break;
    }
    return "no-candidate";
}

struct DryRunSymbol {
    std::string symbol;
    DryRunOutcome outcome = DryRunOutcome::NoCandidate;
    int64_t tournament_ms = 0;   // wall time from tournament start to settle
    int candidates_ok = 0;       // candidate strategies that produced a valid score
    int candidates_total = 0;    // candidate strategies raced
};

// Cumulative history-fetch accounting for the build, sampled as a DELTA across
// it (the source's counters are process-lifetime; the run brackets them).
// cache_served/cache_fetched replaced cache_hits/cache_misses in 0.20.0, and the
// rename is deliberate rather than cosmetic: the old cache_misses counted CACHE
// LOOKUPS, which a pacing-held request repeats on every io_loop pass, so the
// 2026-08-11 run reported cache_misses=967 against hist_requests=36. Renaming
// rather than redefining means an old dry-run line and a new one cannot be
// silently compared — the fields have different names, so the break is visible.
//
// READING THEM: the hit rate is cache_served / (cache_served + cache_fetched +
// held_abandoned). An abandoned request missed the cache and was then errored
// back by the pacing gate, so it is a request the build MADE and neither of the
// first two fields counts it. See net/market_source.h and net/bar_cache.h.
struct DryRunCounters {
    uint64_t cache_served = 0;    // requests answered from cache (fetches avoided)
    uint64_t cache_fetched = 0;   // requests that went to the wire
    uint64_t cache_lookups = 0;   // cache consultations, incl. pacing-held repeats
    uint64_t requests_sent = 0;
    uint64_t held_min_gap = 0;
    uint64_t held_identical = 0;
    uint64_t held_budget = 0;
    uint64_t abandoned = 0;
    uint64_t held_total() const { return held_min_gap + held_identical + held_budget; }
};

struct DryRunSummary {
    int64_t total_ms = 0;    // trigger to settle, wall clock
    size_t pool = 0;         // symbols the scan returned
    size_t delivered = 0;    // pool symbols whose ranking bars arrived
    size_t picks = 0;        // symbols that cleared the price/liquidity gates
    DryRunCounters hist;
    // Empty when the build ran through to admission. Otherwise a short reason
    // token (no spaces) for the phase that gave up: the lineup has five distinct
    // early exits and "it produced nothing" reads identically for all of them.
    std::string abort;
};

// ---------------------------------------------------------------- formatting

namespace dryrun_detail {
inline std::string kv(const char* k, int64_t v) {
    return std::string(k) + '=' + std::to_string(v);
}
inline std::string kv(const char* k, uint64_t v) {
    return std::string(k) + '=' + std::to_string(v);
}
inline std::string kv(const char* k, const std::string& v) {
    // Empty stands in for absent so a parser never sees a bare key. Spaces would
    // split one field into two, so they become '_' rather than being trusted.
    std::string out = std::string(k) + '=';
    if (v.empty()) return out + "-";
    for (char c : v) out += c == ' ' ? '_' : c;
    return out;
}
} // namespace dryrun_detail

// The head of every line: the tag plus the kind of record it is.
inline std::string dryrun_head(const char* kind) {
    return std::string(kDryRunTag) + " kind=" + kind;
}

// The run is starting. Says what it is BEFORE it does anything, so a log that
// gets truncated still shows a dry run was in charge of the process.
inline std::string dryrun_start_line() {
    return dryrun_head("start") +
           " mode=propose-only session=never orders=never";
}

// One phase of the build. `detail` is pre-formatted key=value text (possibly
// empty) describing what the phase produced.
inline std::string dryrun_phase_line(const std::string& phase, int64_t ms,
                                     const std::string& detail) {
    std::string s = dryrun_head("phase") + " phase=" + phase + ' ' +
                    dryrun_detail::kv("ms", ms);
    if (!detail.empty()) s += ' ' + detail;
    return s;
}

// One symbol's tournament, emitted when it settles rather than when it starts —
// the elapsed time is the point.
inline std::string dryrun_tournament_line(const DryRunSymbol& s, size_t idx,
                                          size_t of) {
    return dryrun_head("tournament") + ' ' +
           dryrun_detail::kv("symbol", s.symbol) + ' ' + "idx=" +
           std::to_string(idx) + '/' + std::to_string(of) + ' ' +
           dryrun_detail::kv("ms", s.tournament_ms) + " candidates=" +
           std::to_string(s.candidates_ok) + '/' +
           std::to_string(s.candidates_total) + ' ' +
           dryrun_detail::kv("outcome", dryrun_outcome_name(s.outcome));
}

inline size_t dryrun_count(const std::vector<DryRunSymbol>& syms, DryRunOutcome o) {
    size_t n = 0;
    for (const DryRunSymbol& s : syms)
        if (s.outcome == o) ++n;
    return n;
}

// The verdict a script branches on: zero iff at least one symbol came out of the
// build with parameters fitted to ITSELF this morning. Anything less is a build
// that would have started nothing (or started something on a stale fit), which
// is exactly the outcome a nightly dry run exists to catch. Deliberately NOT
// "did it reach Done" — a build can reach Done having fitted nothing.
inline int dryrun_exit_code(const std::vector<DryRunSymbol>& syms) {
    return dryrun_count(syms, DryRunOutcome::Fitted) > 0 ? 0 : 1;
}

// The one line that has to be readable on its own, because it is the line a
// monitor keeps. Everything a run is judged on is here; the phase lines above
// are the breakdown.
inline std::string dryrun_summary_line(const DryRunSummary& sum,
                                       const std::vector<DryRunSymbol>& syms) {
    using dryrun_detail::kv;
    const int code = dryrun_exit_code(syms);
    int cand_ok = 0, cand_total = 0;
    for (const DryRunSymbol& s : syms) {
        cand_ok += s.candidates_ok;
        cand_total += s.candidates_total;
    }
    std::string list;
    for (const DryRunSymbol& s : syms) {
        if (!list.empty()) list += ',';
        list += s.symbol;
        list += ':';
        list += dryrun_outcome_name(s.outcome);
    }
    std::string out = dryrun_head("summary") + ' ';
    out += kv("result", std::string(code == 0 ? "PASS" : "FAIL"));
    out += ' ' + kv("total_ms", sum.total_ms);
    out += ' ' + kv("abort", sum.abort);
    out += ' ' + kv("pool", static_cast<int64_t>(sum.pool));
    out += ' ' + kv("delivered", static_cast<int64_t>(sum.delivered));
    out += ' ' + kv("picks", static_cast<int64_t>(sum.picks));
    out += ' ' + kv("fitted",
                    static_cast<int64_t>(dryrun_count(syms, DryRunOutcome::Fitted)));
    out += ' ' + kv("own_previous",
                    static_cast<int64_t>(dryrun_count(syms, DryRunOutcome::OwnPrevious)));
    out += ' ' + kv("holding_only",
                    static_cast<int64_t>(dryrun_count(syms, DryRunOutcome::HoldingOnly)));
    out += ' ' + kv("no_candidate",
                    static_cast<int64_t>(dryrun_count(syms, DryRunOutcome::NoCandidate)));
    out += ' ' + kv("excluded",
                    static_cast<int64_t>(dryrun_count(syms, DryRunOutcome::Excluded)));
    out += " candidates_ok=" + std::to_string(cand_ok) + '/' +
           std::to_string(cand_total);
    out += ' ' + kv("cache_served", sum.hist.cache_served);
    out += ' ' + kv("cache_fetched", sum.hist.cache_fetched);
    out += ' ' + kv("cache_lookups", sum.hist.cache_lookups);
    out += ' ' + kv("hist_requests", sum.hist.requests_sent);
    out += ' ' + kv("held", sum.hist.held_total());
    out += ' ' + kv("held_min_gap", sum.hist.held_min_gap);
    out += ' ' + kv("held_identical", sum.hist.held_identical);
    out += ' ' + kv("held_budget", sum.hist.held_budget);
    out += ' ' + kv("held_abandoned", sum.hist.abandoned);
    out += ' ' + kv("symbols", list);
    out += ' ' + kv("exit", static_cast<int64_t>(code));
    return out;
}

} // namespace tt::ui
