// The headless lineup dry run's report format and its verdict.
//
// The whole point of TT_AUTORUN_LINEUP=1 is that a scheduled job on the VPS can
// run a full daily-lineup build and be BELIEVED without a human reading the log,
// so two things have to hold and neither can be checked by running the app:
//
//   - the "dryrun:" lines stay machine-readable (stable tag, no stray spaces
//     inside a value, every field present even when empty), and
//   - the exit code says "no symbol was fitted", not "the build finished".
//
// A build that reaches admission having fitted nothing is exactly the
// 2026-08-10 failure, and it is a SUCCESSFUL-looking build: it logs picks, runs
// six tournaments and reaches Done. If the verdict keyed off completion it would
// pass that day green.
#include "doctest.h"

#include "lineup_dryrun.h"

#include <string>
#include <vector>

using namespace tt::ui;

static DryRunSymbol sym(const char* s, DryRunOutcome o, int ok = 5, int total = 5,
                        int64_t ms = 1000) {
    DryRunSymbol d;
    d.symbol = s;
    d.outcome = o;
    d.candidates_ok = ok;
    d.candidates_total = total;
    d.tournament_ms = ms;
    return d;
}

// Split on spaces: what a `grep dryrun: | awk` reader sees. A value containing a
// space would silently become an extra field, so this is the parse the format
// has to survive.
static std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string field(const std::string& line, const std::string& key) {
    for (const std::string& f : fields(line))
        if (f.rfind(key + "=", 0) == 0) return f.substr(key.size() + 1);
    return "<missing>";
}

// ------------------------------------------------------------------ verdict

TEST_CASE("dryrun: one fitted symbol is a pass, none is a failure") {
    CHECK(dryrun_exit_code({sym("AAPL", DryRunOutcome::Fitted)}) == 0);
    CHECK(dryrun_exit_code({}) == 1);
}

TEST_CASE("dryrun: a build that fitted NOTHING fails even though it completed") {
    // The 2026-08-10 shape: six picks, six tournaments, every one of them left
    // with no fit of its own. Four traded anyway on another symbol's parameters.
    // "It ran to Done" must not be mistaken for "it worked".
    const std::vector<DryRunSymbol> syms{
        sym("SOXS", DryRunOutcome::NoCandidate, 0, 5),
        sym("MUU", DryRunOutcome::Excluded, 0, 5),
        sym("KORU", DryRunOutcome::Excluded, 0, 5),
        sym("SOXL", DryRunOutcome::OwnPrevious, 0, 5),
        sym("SSPC", DryRunOutcome::HoldingOnly, 0, 5)};
    CHECK(dryrun_exit_code(syms) == 1);
    const std::string line = dryrun_summary_line({}, syms);
    CHECK(field(line, "result") == "FAIL");
    CHECK(field(line, "exit") == "1");
    CHECK(field(line, "fitted") == "0");
}

TEST_CASE("dryrun: a stale own fit is NOT counted as fitted this morning") {
    // AdmitOwnPrevious keeps a symbol in the session, so it is a fine outcome
    // for the day — but it is not evidence the build produced anything, which is
    // what the dry run is asked to prove.
    CHECK(dryrun_exit_code({sym("SOXL", DryRunOutcome::OwnPrevious)}) == 1);
    CHECK(dryrun_exit_code({sym("SOXL", DryRunOutcome::OwnPrevious),
                            sym("AAPL", DryRunOutcome::Fitted)}) == 0);
}

// ------------------------------------------------------------------- format

TEST_CASE("dryrun: every line carries the grep tag and names its kind") {
    struct Case { std::string line, kind; };
    const std::vector<Case> lines{
        {dryrun_start_line(), "start"},
        {dryrun_phase_line("scan", 12, "pool=30"), "phase"},
        {dryrun_tournament_line(sym("AAPL", DryRunOutcome::Fitted), 1, 6),
         "tournament"},
        {dryrun_summary_line({}, {}), "summary"}};
    for (const Case& c : lines) {
        CHECK(c.line.rfind("dryrun:", 0) == 0);
        CHECK(field(c.line, "kind") == c.kind);
    }
}

TEST_CASE("dryrun: a phase line states its elapsed ms") {
    const std::string l = dryrun_phase_line("rank", 4321, "picks=6");
    CHECK(field(l, "phase") == "rank");
    CHECK(field(l, "ms") == "4321");
    CHECK(field(l, "picks") == "6");
    // No detail is still a well-formed line, not a trailing space.
    const std::string bare = dryrun_phase_line("scan", 7, "");
    CHECK(bare == "dryrun: kind=phase phase=scan ms=7");
}

TEST_CASE("dryrun: a tournament line carries symbol, position, ms and outcome") {
    const std::string l =
        dryrun_tournament_line(sym("TQQQ", DryRunOutcome::Fitted, 4, 5, 48210), 2, 6);
    CHECK(field(l, "symbol") == "TQQQ");
    CHECK(field(l, "idx") == "2/6");
    CHECK(field(l, "ms") == "48210");
    CHECK(field(l, "candidates") == "4/5");
    CHECK(field(l, "outcome") == "fitted");
}

TEST_CASE("dryrun: the summary reports counters, per-symbol outcomes and totals") {
    DryRunSummary s;
    s.total_ms = 345678;
    s.pool = 30;
    s.delivered = 28;
    s.picks = 6;
    s.hist.cache_hits = 18;
    s.hist.cache_misses = 42;
    s.hist.requests_sent = 42;
    s.hist.held_min_gap = 7;
    s.hist.held_identical = 1;
    s.hist.held_budget = 2;
    s.hist.abandoned = 3;
    const std::vector<DryRunSymbol> syms{
        sym("AAPL", DryRunOutcome::Fitted, 5, 5),
        sym("TQQQ", DryRunOutcome::Fitted, 4, 5),
        sym("SOXS", DryRunOutcome::Excluded, 0, 5)};
    const std::string l = dryrun_summary_line(s, syms);
    CHECK(field(l, "result") == "PASS");
    CHECK(field(l, "total_ms") == "345678");
    CHECK(field(l, "pool") == "30");
    CHECK(field(l, "delivered") == "28");
    CHECK(field(l, "picks") == "6");
    CHECK(field(l, "fitted") == "2");
    CHECK(field(l, "excluded") == "1");
    CHECK(field(l, "candidates_ok") == "9/15");
    CHECK(field(l, "cache_hits") == "18");
    CHECK(field(l, "cache_misses") == "42");
    CHECK(field(l, "hist_requests") == "42");
    // held is the sum of the three reasons, so a reader can take one number or
    // the breakdown without adding them up wrong.
    CHECK(field(l, "held") == "10");
    CHECK(field(l, "held_min_gap") == "7");
    CHECK(field(l, "held_identical") == "1");
    CHECK(field(l, "held_budget") == "2");
    CHECK(field(l, "held_abandoned") == "3");
    CHECK(field(l, "symbols") == "AAPL:fitted,TQQQ:fitted,SOXS:excluded");
    CHECK(field(l, "exit") == "0");
}

TEST_CASE("dryrun: an aborted build names the phase that gave up") {
    DryRunSummary s;
    s.total_ms = 30012;
    s.abort = "scan-timeout";
    const std::string l = dryrun_summary_line(s, {});
    CHECK(field(l, "abort") == "scan-timeout");
    CHECK(field(l, "result") == "FAIL");
    // A completed build still emits the key, so a parser never has to cope with
    // a field that is sometimes absent.
    CHECK(field(dryrun_summary_line({}, {}), "abort") == "-");
}

TEST_CASE("dryrun: no value can smuggle a space into the field stream") {
    // The abort reason is the one field built from free-ish text. If it ever
    // carried a space it would parse as an extra field and shift nothing else —
    // silently, which is the worst kind of format bug.
    DryRunSummary s;
    s.abort = "scan timed out";
    const std::string l = dryrun_summary_line(s, {});
    CHECK(field(l, "abort") == "scan_timed_out");
}

TEST_CASE("dryrun: every field after the tag is a key=value pair") {
    // The contract an `awk -F' '` reader depends on. It was NOT true of the
    // first draft — the summary opened with a bare "summary" marker — and a bare
    // word shifts nothing, so a parser splitting on '=' would have silently read
    // one field short forever.
    DryRunSummary s;
    s.total_ms = 1;
    const std::vector<std::string> lines{
        dryrun_start_line(), dryrun_phase_line("fetch_bars", 15021, "delivered=28/30"),
        dryrun_tournament_line(sym("AAPL", DryRunOutcome::Fitted), 1, 6),
        dryrun_summary_line(s, {sym("AAPL", DryRunOutcome::Fitted)})};
    for (const std::string& l : lines) {
        const std::vector<std::string> f = fields(l);
        REQUIRE(f.size() >= 2);
        CHECK(f[0] == "dryrun:");
        for (size_t i = 1; i < f.size(); ++i)
            CHECK(f[i].find('=') != std::string::npos);
    }
}
