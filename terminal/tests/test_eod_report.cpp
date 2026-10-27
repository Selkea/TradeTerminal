// The end-of-day summary page.
//
// This is the first alert in the app whose TEXT is testable — every other one is
// a string built inline inside a pump, which is why "the page said the wrong
// number" has never been catchable before it shipped. The cases below are the
// ones where a plausible implementation lies to the operator: an unopened
// journal reported as a day that did not trade, a gross P&L printed where a net
// one was promised, and a summary that files itself under Risk because it
// happens to contain the word "halted".
#include "doctest.h"

#include "alert_rules.h"
#include "eod_report.h"
#include "journal.h"   // EquityPoint, RoiBasis, roi_from_sessions

#include <string>
#include <vector>

using tt::ui::AlertCategory;
using tt::ui::classify_alert_category;
using tt::ui::EodReport;
using tt::ui::EodSymbolRow;
using tt::ui::eod_allocated;
using tt::ui::eod_deployed_peak;   // an empty {} arg has no type for ADL to find
using tt::ui::EquityPoint;
using tt::ui::format_eod_report;
using tt::ui::RoiBasis;
using tt::ui::roi_from_sessions;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// 2026-08-26 as it actually happened on the VPS, which is where every number
// below comes from: SNDQ round-tripped once and re-entered, SPCH and DAIC opened
// and were carried overnight, MRNA armed a breakout that never triggered and
// SNXX never signalled.
EodReport live_day() {
    EodReport r;
    r.date = "2026-08-26";
    r.sessions = 1;
    r.fills = 6;
    r.net = -248.15;
    r.daily_loss = 248.15;
    r.daily_loss_limit = 2000.0;
    r.symbols = {
        {"SNXX", 0.0, 0.0, 0, 0.0, 0.0},
        {"MRNA", 0.0, 0.0, 0, 0.0, 0.0},
        {"SNDQ", 30.71, 4.5686, 293, 17.02, -117.20},
        {"SPCH", -2.8917, 2.8917, 578, 8.65, -63.58},
        {"DAIC", -5.0130, 5.0130, 1002, 5.00, -90.18},
    };
    return r;
}

} // namespace

TEST_CASE("eod report: the day's headline numbers all reach the page") {
    const std::string s = format_eod_report(live_day());
    CHECK(has(s, "DAY SUMMARY 2026-08-26"));
    CHECK(has(s, "net -248.15 on 6 fills, 1 session"));
    // The split, which is the whole point: a day that banked +22.81 while
    // carrying -270.96 of open loss is neither a +22 day nor a -271 day.
    CHECK(has(s, "realized     +22.81"));
    CHECK(has(s, "open        -270.96"));
    CHECK(has(s, "3 positions held overnight"));
    CHECK(has(s, "risk: 248.15 of the 2000 daily-loss limit (12%)"));
}

TEST_CASE("eod report: per-symbol rows carry the carried position") {
    const std::string s = format_eod_report(live_day());
    CHECK(has(s, "SNDQ     +30.71 realized | 293 @ 17.02, -117.20 open"));
    CHECK(has(s, "DAIC      -5.01 realized | 1002 @ 5.00, -90.18 open"));
    // Symbols that neither traded nor hold anything collapse to one line rather
    // than three-quarters of a phone screen of zeroes.
    CHECK(has(s, "no trades: SNXX, MRNA"));
    CHECK_FALSE(has(s, "SNXX      +0.00"));
}

TEST_CASE("eod report: fees are reported and realized is already net of them") {
    EodReport r = live_day();
    const std::string s = format_eod_report(r);
    // 4.5686 + 2.8917 + 5.0130 = 12.47.
    CHECK(has(s, "(12.47 in fees)"));
    // The contract of the field, pinned so a caller that hands over the GROSS
    // figure shows up here rather than on the phone. SNDQ's gross was +35.28;
    // net of its 4.57 commission it is +30.71, and it is the net one that
    // answers "what did the day bank".
    CHECK(has(s, "+30.71 realized"));
    CHECK_FALSE(has(s, "+35.28"));
}

TEST_CASE("eod report: eod_row_from converts the engine's GROSS figure to net") {
    // SNDQ on 2026-08-26: bought 294 @ 16.99, sold @ 17.11. The engine books
    // +35.28 and charges the 4.57 commission to cash separately, so the page's
    // "realized" must be 30.71 — the difference is 12.9% of the trade.
    const EodSymbolRow r = tt::ui::eod_row_from("SNDQ", 35.28, 4.5686, 0, 0.0, 0.0);
    CHECK(r.realized_net == doctest::Approx(30.7114));
    CHECK(r.fees == doctest::Approx(4.5686));
    CHECK(r.symbol == "SNDQ");
    // A losing trade pays commission too: the fee deepens the loss, never
    // offsets it. Sign errors here are invisible in the happy case above.
    const EodSymbolRow loss = tt::ui::eod_row_from("DAIC", -20.0, 5.0, 0, 0.0, 0.0);
    CHECK(loss.realized_net == doctest::Approx(-25.0));
    // A symbol that only opened a position has no realized P&L but HAS paid to
    // get in, which is why 2026-08-26 showed SPCH and DAIC slightly negative
    // before either had closed anything.
    const EodSymbolRow opened = tt::ui::eod_row_from("SPCH", 0.0, 2.8917, 578, 8.65, -63.58);
    CHECK(opened.realized_net == doctest::Approx(-2.8917));
    CHECK(opened.qty == 578);
    CHECK(opened.unrealized == doctest::Approx(-63.58));
}

TEST_CASE("eod report: a day nothing traded says so") {
    EodReport r;
    r.date = "2026-08-21";
    // The report only fires on a day the market opened, so zero sessions is not
    // "the app was off" — it is a trading day that passed with nothing running.
    // 2026-08-21 was exactly that and nothing anywhere announced it.
    const std::string s = format_eod_report(r);
    CHECK(has(s, "NO SESSION RAN TODAY"));
    CHECK_FALSE(has(s, "net +0.00"));
}

TEST_CASE("eod report: an unopened journal is UNKNOWN, never a quiet day") {
    EodReport r;
    r.date = "2026-08-26";
    r.journal_available = false;
    r.symbols = {{"SNDQ", 30.71, 4.57, 293, 17.02, -117.20}};
    const std::string s = format_eod_report(r);
    // The failure this guards: journal.db not opening leaves sessions/fills/net
    // at zero, and a formatter that reads zero sessions as "nothing traded"
    // turns a storage fault into a false all-clear on a day that DID trade.
    CHECK(has(s, "net unavailable"));
    CHECK(has(s, "journal.db is not open"));
    CHECK_FALSE(has(s, "NO SESSION RAN TODAY"));
    // The half that is still knowable is still reported.
    CHECK(has(s, "SNDQ     +30.71 realized"));
}

TEST_CASE("eod report: a live session with no journal row is a journalling fault") {
    EodReport r;
    r.date = "2026-08-26";
    r.journal_available = true;   // the db opened...
    r.sessions = 0;               // ...and has no row for a session that ran
    r.symbols = {{"SNDQ", 30.71, 4.57, 293, 17.02, -117.20}};
    const std::string s = format_eod_report(r);
    CHECK(has(s, "no row for today"));
    // Distinct from the empty-day message, because they call for opposite
    // responses: one is a quiet market, the other is the authoritative trade
    // record missing a session that demonstrably happened.
    CHECK_FALSE(has(s, "NO SESSION RAN TODAY"));
}

TEST_CASE("eod report: a second session is disclosed, not silently summed") {
    EodReport r = live_day();
    r.sessions = 2;   // a mid-day lineup swap opens a new one
    const std::string s = format_eod_report(r);
    CHECK(has(s, "2 sessions"));
    // realized+open comes from the engine, which can only speak for the session
    // that was live at the close, while `net` covers both. Saying so beats
    // printing two numbers that do not add up and letting the reader guess.
    CHECK(has(s, "last session only"));
}

TEST_CASE("eod report: a halt leads the page") {
    EodReport r = live_day();
    r.halted = true;
    const std::string s = format_eod_report(r);
    CHECK(has(s, "HALTED"));
    // Above the table: a day that hit its loss limit is not one you read the
    // per-symbol breakdown of first.
    CHECK(s.find("HALTED") < s.find("SNDQ"));
}

TEST_CASE("eod report: a profitable day reports no loss-limit usage") {
    EodReport r = live_day();
    r.net = 412.90;
    r.daily_loss = -412.90;   // risk.daily_loss is positive only when DOWN
    r.symbols = {{"SNDQ", 412.90, 6.10, 0, 0.0, 0.0}};
    const std::string s = format_eod_report(r);
    CHECK(has(s, "risk: 0.00 of the 2000 daily-loss limit (0%)"));
    // The bug this pins: passing the signed figure straight through prints a
    // NEGATIVE percentage of the loss limit on a winning day.
    CHECK_FALSE(has(s, "-412.90 of the"));
    CHECK_FALSE(has(s, "(-21%)"));
}

TEST_CASE("eod report: no armed loss limit means no risk line, not a divide by zero") {
    EodReport r = live_day();
    r.daily_loss_limit = 0.0;
    const std::string s = format_eod_report(r);
    CHECK_FALSE(has(s, "daily-loss limit"));
    CHECK(has(s, "net -248.15"));
}

TEST_CASE("eod report: a flat symbol that traded still gets its own row") {
    EodReport r;
    r.date = "2026-08-26";
    r.sessions = 1;
    r.fills = 2;
    r.net = 30.71;
    r.symbols = {{"SNDQ", 30.71, 4.57, 0, 0.0, 0.0}};
    const std::string s = format_eod_report(r);
    CHECK(has(s, "SNDQ     +30.71 realized | flat"));
    CHECK_FALSE(has(s, "no trades: SNDQ"));
    CHECK(has(s, "0 positions held overnight"));
}

// ---------------------------------------------------------------------------
// Categorisation. The summary is the one alert whose body deliberately quotes
// every other category's vocabulary, so this is where a substring collision
// would land — and the consequence is not cosmetic: Risk is the category the UI
// marks as unsilenceable-by-design, so a daily digest filed there becomes noise
// in the one channel that must never be ignored.
// ---------------------------------------------------------------------------

TEST_CASE("eod report: the summary categorises as EndOfDay") {
    CHECK(classify_alert_category(format_eod_report(live_day())) ==
          AlertCategory::EndOfDay);
}

TEST_CASE("eod report: quoting a halt does not refile the summary under Risk") {
    EodReport r = live_day();
    r.halted = true;
    const std::string s = format_eod_report(r);
    CHECK(classify_alert_category(s) == AlertCategory::EndOfDay);
}

TEST_CASE("eod report: the tag wins over every other category's vocabulary") {
    // Each of these substrings routes a normal log line somewhere else; prefixed
    // with the tag they must all still be EndOfDay. This is the test that fails
    // if the tag check is ever moved below the branches it protects against.
    const char* traps[] = {
        "RISK HALT (daily loss limit)",
        "EOD BACKSTOP - open position(s) past 15:57",
        "KILL SWITCH engaged",
        "SNDQ unprotected",
        "lineup: EXCLUDED MSTZ",
        "session guard: stopping a live session",
        "live: SNDQ fill BUY 100 @ 16.99 (order #2)",
        "gateway disconnected",
    };
    for (const char* t : traps) {
        const std::string tagged = std::string(tt::ui::kEodReportTag) + " 2026-08-26\n" + t;
        CHECK(classify_alert_category(tagged) == AlertCategory::EndOfDay);
        // ...and the same text WITHOUT the tag still goes where it always did,
        // so this proves the tag is doing the work rather than the branches
        // having been broken.
        CHECK(classify_alert_category(t) != AlertCategory::EndOfDay);
    }
}

TEST_CASE("eod report: EndOfDay is not treated as a safety category") {
    // Risk and Integrity are marked (!) in the menu and warn about what silence
    // costs. A daily digest is not in that class — it is exactly the kind of
    // routine page an operator should be able to turn off without a warning.
    CHECK_FALSE(tt::ui::alert_category_is_safety(AlertCategory::EndOfDay));
    CHECK(tt::ui::alert_category_is_safety(AlertCategory::Risk));
}

TEST_CASE("eod report: the category has a name of its own in /diag") {
    // /diag reports muted categories BY NAME so the answer survives a
    // reordering of the enum. A new enumerator that falls through to the
    // default arm would report itself as "System" and the operator would be
    // told the wrong switch swallowed their page.
    CHECK(std::string(tt::ui::alert_category_name(AlertCategory::EndOfDay)) ==
          "EndOfDay");
    CHECK(std::string(tt::ui::alert_category_name(AlertCategory::System)) ==
          "System");
}

// ---------------------------------------------------------------------------
// RETURN ON CAPITAL (0.48.0).
//
// The denominator is the whole question. This is a ~$1M paper balance against a
// $5,000 per-symbol notional cap, so on 2026-10-26 the same +$7,883.35 since
// 2026-08-03 is +0.79% of the account and roughly +26% of the ~$30k the strategy
// actually commits. Report only the first and a working system reads as rounding
// error; report only the second and it hides that 97% of the account is idle.
// Both are reported, both labelled.
// ---------------------------------------------------------------------------
namespace {
// 2026-10-26 as it actually stood: equity 1,002,794.09, inception baseline
// 994,910.74 from 2026-08-03 over 67 closed sessions.
EodReport roi_day() {
    EodReport r;
    r.date = "2026-10-26";
    r.sessions = 1;
    r.fills = 4;
    r.net = 273.97;
    r.daily_loss_limit = 2000.0;
    r.symbols = {
        {"SNXX", 180.20, 3.10, 0, 0.0, 0.0, 14900.0, 5000.0},
        {"KORU", 93.77, 2.05, 0, 0.0, 0.0, 14887.0, 5000.0},
        {"MUU", 0.0, 0.0, 0, 0.0, 0.0, 0.0, 5000.0},
        {"SOXL", 0.0, 0.0, 0, 0.0, 0.0, 0.0, 5000.0},
        {"IREN", 0.0, 0.0, 0, 0.0, 0.0, 0.0, 5000.0},
        {"CRWV", 0.0, 0.0, 0, 0.0, 0.0, 0.0, 5000.0},
    };
    r.deployed_peak = eod_deployed_peak(r.symbols);
    r.allocated = eod_allocated(r.symbols);
    r.roi_ok = true;
    r.roi_from = "2026-08-03";
    r.roi_baseline = 994910.74;
    r.roi_equity = 1002794.09;
    r.roi_sessions = 67;
    r.mtd_ok = true;
    r.mtd_from = "2026-10-01";
    r.mtd_baseline = 1002084.00;
    return r;
}
} // namespace

TEST_CASE("roi: today is measured against capital actually deployed") {
    const std::string s = format_eod_report(roi_day());
    // 6 x $5,000 allocated; 273.97 / 30,000 = 0.91%.
    CHECK(has(s, "return on the 30,000 allocated"));
    CHECK(has(s, "today       +273.97  = +0.91%"));
    // NOT the account base for the daily line: 273.97/1,002,794 is +0.03%, the
    // number that makes a real day look like noise.
    CHECK_FALSE(has(s, "today       +273.97  = +0.03%"));
    // And NOT the day's own deployment either. 273.97/29,787 = 0.92% - close
    // here by coincidence, but that denominator moves with how many signals
    // fired, which is not a fact about performance.
    CHECK_FALSE(has(s, "today       +273.97  = +0.92%"));
    // Deployment is reported as UTILISATION instead: 29,787/30,000 = 99%.
    CHECK(has(s, "used         29,787 at peak = 99% of it"));
}

TEST_CASE("roi: the cumulative lines use ALLOCATED and name their base") {
    const std::string s = format_eod_report(roi_day());
    // 7,883.35 on the 30,000 allocated = +26.28%. THIS is the figure that
    // answers "is this worth the capital it ties up".
    CHECK(has(s, "overall    +7883.35  = +26.28%  (from 2026-08-03, 67 sessions)"));
    CHECK(has(s, "month       +710.09  = +2.37%  (from 2026-10-01)"));
    // The account view is kept alongside, precisely so the page cannot be
    // accused of quoting only the flattering base: the same money is +0.79% of
    // a ~$1M balance that is 97% idle.
    CHECK(has(s, "account    +7883.35  = +0.79% of 1,002,794 equity"));
    // THE BASELINE DATE IS PRINTED. It is chosen by a rule (roi_from_sessions
    // skips the 100,000 placeholder era), and a rule whose choice is invisible
    // is a rule that can inflate a percentage unnoticed.
    CHECK(has(s, "from 2026-08-03, 67 sessions"));
    CHECK(has(s, "of 1,002,794 equity"));
}

TEST_CASE("roi: a missing baseline says so instead of printing nothing") {
    EodReport r = roi_day();
    r.roi_ok = false;
    r.mtd_ok = false;
    const std::string s = format_eod_report(r);
    CHECK(has(s, "overall  not available - no believable equity baseline yet"));
    // An omitted line reads as zero to anyone skimming. Today still reports,
    // because the allocated base needs no equity history at all.
    CHECK(has(s, "today       +273.97  = +0.91%"));
    // No account line at all - "equity" itself still appears, in the
    // not-available message, so the assertion has to name the line.
    CHECK_FALSE(has(s, "account "));
    CHECK_FALSE(has(s, "overall  +"));
}

TEST_CASE("roi: nothing deployed is NOT a zero denominator") {
    // A day with no live session at the crossing has deployed_peak 0. Dividing
    // by it would print inf% or nan%; the line must simply be absent while the
    // cumulative ones, which need no daily denominator, still report.
    EodReport r = roi_day();
    r.symbols.clear();
    r.deployed_peak = 0.0;
    r.allocated = 0.0;
    const std::string s = format_eod_report(r);
    CHECK_FALSE(has(s, "at peak"));
    CHECK_FALSE(has(s, "inf"));
    CHECK_FALSE(has(s, "nan"));
    // The ACCOUNT line needs no daily denominator, so it still reports.
    CHECK(has(s, "account    +7883.35  = +0.79%"));
}

TEST_CASE("roi: a losing day reports a negative return, not an absolute one") {
    EodReport r = roi_day();
    r.net = -150.37;
    r.symbols = {{"MUU", -158.32, 2.12, 0, 0.0, 0.0, 4548.0, 5000.0}};
    r.deployed_peak = eod_deployed_peak(r.symbols);
    r.allocated = eod_allocated(r.symbols);
    const std::string s = format_eod_report(r);
    // A ONE-SYMBOL lineup allocates 5,000, so -150.37 / 5,000 = -3.01%. The
    // sign has to survive: on 2026-09-28 this was the whole day's loss, on one
    // unexitable MUU short.
    CHECK(has(s, "today       -150.37  = -3.01%"));
    CHECK(has(s, "used          4,548 at peak = 91% of it"));
}

TEST_CASE("roi: deployed sums high-water marks, allocated sums the ceilings") {
    const std::vector<EodSymbolRow> rows = {
        {"A", 0, 0, 0, 0, 0, 5000.0, 5000.0},
        {"B", 0, 0, 0, 0, 0, 4900.0, 5000.0},
        {"C", 0, 0, 0, 0, 0, 0.0, 5000.0},   // in the lineup, never took a position
    };
    CHECK(eod_deployed_peak(rows) == doctest::Approx(9900.0));
    // ALLOCATED counts C's ceiling even though it never traded: the capital was
    // reserved for it and unavailable elsewhere, which is what makes this a
    // denominator that does not move with how many signals fired.
    CHECK(eod_allocated(rows) == doctest::Approx(15000.0));
    CHECK(eod_deployed_peak({}) == doctest::Approx(0.0));
    CHECK(eod_allocated({}) == doctest::Approx(0.0));
}

// ---------------------------------------------------------------------------
// The equity baseline rule. roi_from_sessions is where the 100,000 placeholder
// era gets skipped, and getting it wrong reports roughly +900%.
// ---------------------------------------------------------------------------
TEST_CASE("roi basis: the 100k placeholder era is skipped") {
    // This journal's real shape: 48 closed sessions carrying a 100,000
    // placeholder (begin_session can only record one on the TWS route; a later
    // set_baseline re-anchors it), then the real ~$1M era from 2026-08-03.
    std::vector<EquityPoint> closed;
    for (int i = 0; i < 48; ++i)
        closed.push_back({"2026-07-28", 100000.0, 100000.0});
    closed.push_back({"2026-08-03", 994910.74, 995200.0});
    closed.push_back({"2026-10-26", 1002520.12, 1002794.09});

    const RoiBasis b = roi_from_sessions(closed);
    REQUIRE(b.ok);
    CHECK(b.from_date == "2026-08-03");
    CHECK(b.baseline_equity == doctest::Approx(994910.74));
    CHECK(b.latest_equity == doctest::Approx(1002794.09));
    CHECK(b.sessions == 2);                    // the placeholders are not counted
    // The bug this prevents: measuring from 100,000 to 1,002,794 is +902%.
    const double wrong = (b.latest_equity - 100000.0) / 100000.0 * 100.0;
    CHECK(wrong > 900.0);
    const double right = (b.latest_equity - b.baseline_equity) / b.baseline_equity * 100.0;
    CHECK(right == doctest::Approx(0.79).epsilon(0.02));
}

TEST_CASE("roi basis: month-to-date comes from the LATEST session's month") {
    std::vector<EquityPoint> closed = {
        {"2026-08-03", 994910.74, 995200.0},
        {"2026-09-30", 1001900.00, 1002000.0},
        {"2026-10-01", 1002084.00, 1002200.0},
        {"2026-10-26", 1002520.12, 1002794.09},
    };
    const RoiBasis b = roi_from_sessions(closed);
    REQUIRE(b.mtd_ok);
    // The first OCTOBER session, because the latest session is in October -
    // taken from the data rather than from today's clock, so a report emitted
    // either side of midnight cannot name a period it did not measure.
    CHECK(b.mtd_from_date == "2026-10-01");
    CHECK(b.mtd_baseline_equity == doctest::Approx(1002084.00));
    CHECK(b.from_date == "2026-08-03");        // inception is unaffected
    CHECK(b.sessions == 4);
}

TEST_CASE("roi basis: no usable history says so rather than guessing") {
    CHECK_FALSE(roi_from_sessions({}).ok);
    // Every baseline implausible against the latest equity: refuse.
    const RoiBasis all_ph = roi_from_sessions({
        {"2026-07-08", 100000.0, 100000.0},
        {"2026-07-28", 100000.0, 1002794.09},
    });
    CHECK_FALSE(all_ph.ok);
    // A zero/absent latest equity cannot anchor anything either.
    CHECK_FALSE(roi_from_sessions({{"2026-08-03", 994910.74, 0.0}}).ok);
}

TEST_CASE("roi: with nothing deployed AND no baseline the block is absent") {
    // Not an empty "return" heading with nothing under it. The earlier
    // nothing-deployed case still had a baseline, so it never exercised the
    // outer guard - mutation testing found it surviving.
    EodReport r = roi_day();
    r.symbols.clear();
    r.deployed_peak = 0.0;
    r.allocated = 0.0;
    r.roi_ok = false;
    r.mtd_ok = false;
    const std::string s = format_eod_report(r);
    CHECK_FALSE(has(s, "return"));
    CHECK_FALSE(has(s, "deployed"));
    CHECK_FALSE(has(s, "not available"));
    CHECK(has(s, "net "));            // the rest of the page is untouched
}

TEST_CASE("roi: a negative equity still prints its sign") {
    // Unlikely, and precisely why it is worth pinning: money() formats the
    // magnitude, and a blown account printing as a positive balance would be a
    // silently wrong number on the one line showing the account total.
    EodReport r = roi_day();
    r.roi_equity = -1500.0;
    r.roi_baseline = 1000.0;
    const std::string s = format_eod_report(r);
    CHECK(has(s, "of -1,500 equity"));
}

TEST_CASE("roi basis: a baseline far ABOVE the latest equity is refused too") {
    // Only checking the low side would accept a stale or mis-scaled figure and
    // report a catastrophic loss that never happened.
    const RoiBasis b = roi_from_sessions({
        {"2026-08-03", 10000000.0, 1002794.09},   // 10x too HIGH
        {"2026-10-26", 1002520.12, 1002794.09},
    });
    REQUIRE(b.ok);
    CHECK(b.from_date == "2026-10-26");           // the 10M row is skipped
    CHECK(b.baseline_equity == doctest::Approx(1002520.12));
    CHECK(b.sessions == 1);
}

TEST_CASE("roi basis: a zero baseline is refused, not divided by") {
    const RoiBasis b = roi_from_sessions({
        {"2026-08-03", 0.0, 1002794.09},
        {"2026-10-26", 1002520.12, 1002794.09},
    });
    REQUIRE(b.ok);
    CHECK(b.from_date == "2026-10-26");
    CHECK(b.baseline_equity == doctest::Approx(1002520.12));
}

TEST_CASE("roi basis: a placeholder inside the CURRENT month is skipped too") {
    // The placeholder era is historical, but the same rule has to hold for the
    // month-to-date baseline - otherwise one bad row in the latest month
    // reports a ~+900% month.
    const RoiBasis b = roi_from_sessions({
        {"2026-08-03", 994910.74, 995200.0},
        {"2026-10-01", 100000.0, 1002200.0},      // a placeholder, this month
        {"2026-10-05", 1002084.00, 1002300.0},
        {"2026-10-26", 1002520.12, 1002794.09},
    });
    REQUIRE(b.mtd_ok);
    CHECK(b.mtd_from_date == "2026-10-05");       // NOT 2026-10-01
    CHECK(b.mtd_baseline_equity == doctest::Approx(1002084.00));
    const double mtd = (b.latest_equity - b.mtd_baseline_equity) /
                       b.mtd_baseline_equity * 100.0;
    CHECK(mtd == doctest::Approx(0.07).epsilon(0.1));
}

TEST_CASE("roi basis: a single session is enough, and is its own baseline") {
    const RoiBasis b = roi_from_sessions({{"2026-08-03", 994910.74, 995200.0}});
    REQUIRE(b.ok);
    CHECK(b.from_date == "2026-08-03");
    CHECK(b.sessions == 1);
    CHECK(b.latest_equity == doctest::Approx(995200.0));
    CHECK(b.mtd_ok);                           // August, same month as itself
}
