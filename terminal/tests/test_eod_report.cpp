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

#include <string>

using tt::ui::AlertCategory;
using tt::ui::classify_alert_category;
using tt::ui::EodReport;
using tt::ui::EodSymbolRow;
using tt::ui::format_eod_report;

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
