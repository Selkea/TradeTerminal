#pragma once
// The end-of-day summary: what the trading day made, what it is still holding,
// and how close it came to the loss limit — as one page, once, after the close.
//
// WHY THIS EXISTS AS ITS OWN FILE. The formatting is a pure function of a plain
// struct, so the suite can assert on the text without constructing an App, an
// Engine, a journal or a notifier. Every other alert in this app is a string
// built at its call site inside a pump, which is why none of them have tests and
// why "the page said the wrong number" has never been catchable before it shipped.
//
// WHAT THE NUMBERS MEAN, because two of them are easy to confuse:
//
//   net       the JOURNAL's figure for the day: sum over the day's sessions of
//             (final_equity - initial_cash). It covers every session the day
//             ran, and it includes the mark on positions still open. This is
//             the number the Journal panel's Day table shows, so the page and
//             the panel cannot disagree.
//   realized  closed trades only, NET of commissions, for the session that was
//             live when the report fired. This is what the day actually banked.
//   open      mark-to-market on the positions carried into tomorrow.
//
// realized + open == net only when the day ran exactly one session and its
// baseline was re-anchored (TradeJournal::set_baseline). A lineup swap opens a
// second session, and then the engine can only speak for the last one — so the
// report says which case it is rather than publishing a sum that does not add up.
//
// The operator's day ends on the 16:15 session guard, which KEEPS positions.
// That makes "what did today make" genuinely ambiguous — a day that banked $22
// while carrying $271 of open loss is not a $22 day and is not a -$271 day —
// and it is why the split above is reported instead of a single figure.

#include <string>
#include <vector>

namespace tt::ui {

// Matched FIRST by classify_alert_category, before the Risk branch, so the
// summary can quote any word it likes — "halted", "unprotected", "EOD" — without
// being re-filed under a category it is not. The same defence, and for the same
// reason, as kUnreachableStopDeclinedTag. It is a visible prefix rather than a
// hidden marker because it is also the page's title on the phone.
inline constexpr const char* kEodReportTag = "DAY SUMMARY";

struct EodSymbolRow {
    std::string symbol;
    double realized_net = 0;   // closed trades, minus this symbol's commissions
    double fees = 0;
    double qty = 0;            // position carried into the next session
    double avg_price = 0;
    double unrealized = 0;     // mark on that carried position
    // This symbol's high-water capital for the session (SymbolState::
    // peak_notional). Not the closing position - the book is flat by the close,
    // so that would be zero on every day that traded.
    //
    // LAST, deliberately. Every case in test_eod_report.cpp brace-initialises
    // this struct positionally, so a field added anywhere above here silently
    // reassigns six existing columns. Appending leaves them reading what they
    // say and defaults this to 0, which is also the honest value for a row
    // built before the measurement existed.
    double peak_notional = 0;
    // The notional ceiling in force for this symbol. Summed across the
    // lineup this is the capital ALLOCATED to the strategy.
    double max_notional = 0;
};

struct EodReport {
    std::string date;          // local YYYY-MM-DD, the day being reported
    // journal.db opened. When it did not, `sessions`/`fills`/`net` are not
    // absent-meaning-zero, they are UNKNOWN — and the difference matters,
    // because zero sessions is itself a headline this report exists to raise.
    // A disabled journal must never be reported as a day that did not trade.
    bool journal_available = true;
    // Straight from TradeJournal::days() for `date`. sessions == 0 means no live
    // session ran at all today, which on a trading day is itself the news.
    int sessions = 0;
    int fills = 0;
    double net = 0;
    // The engine's live snapshot, taken at the close crossing BEFORE the guard
    // stops the session — after the stop there is no session left to ask.
    // Empty when nothing was running by then.
    std::vector<EodSymbolRow> symbols;
    double daily_loss = 0;         // risk.daily_loss at the crossing
    double daily_loss_limit = 0;   // 0 = no limit armed
    bool halted = false;
    // The journal row for this day was still open when the report was emitted:
    // the session did not stop within the grace window, so `net` is missing that
    // session's contribution. Said out loud rather than quietly under-reported.
    bool session_still_open = false;

    // ---- return on capital -------------------------------------------
    //
    // THE DENOMINATOR IS THE WHOLE QUESTION, so two are reported and both
    // are labelled. This is a ~$1M paper balance against a $5,000 per-symbol
    // notional cap, so on 2026-10-26 the SAME +$7,883 since 2026-08-03 is
    // +0.79% of the account and roughly +26% of the ~$30k the strategy
    // actually commits. Report only the first and a working system reads as
    // rounding error; report only the second and it hides that 97% of the
    // account is idle. Neither is wrong. Either alone misleads.

    // Peak concurrent notional across every symbol today, summed from
    // SymbolState::peak_notional. 0 = unknown (no session was live at the
    // crossing), which must read as "not measured", never as "nothing
    // deployed" - that would make today's return look infinite.
    double deployed_peak = 0;
    // Capital ALLOCATED: the sum of the live symbols' notional ceilings.
    //
    // THIS IS THE RETURN DENOMINATOR, and the reasoning is worth keeping.
    // Three candidates were on the table:
    //
    //   account balance (~$1,002,794) - what the brokerage balance did, but
    //       the strategy never commits more than ~$30k of it. Move the rest
    //       to a money-market fund and it trades identically, so that
    //       capital is not invested in this. A $274 day reads +0.03%.
    //   deployed THAT DAY ($23k-45k, varies) - the first cut used this and
    //       it is the wrong base for a RETURN. A day deploying $4,548 and
    //       losing $158 reads -3.5%; six symbols deploying $45k and losing
    //       the same $158 reads -0.35%. Same strategy, same loss, 10x the
    //       number - driven by how many signals happened to fire, which is
    //       not a fact about performance. A denominator that moves for
    //       reasons unrelated to the numerator makes the series
    //       incomparable, which is the one thing a return series is for.
    //   ALLOCATED (sum of the caps) - stable day to day, derived from the
    //       operator's own risk config rather than from the balance, and
    //       conservative: allocated >= deployed, so it never flatters.
    //
    // Summed per live symbol rather than top_n x cap, so a day the lineup
    // admitted two symbols instead of six is measured against the two.
    //
    // CAVEAT, stated because the figure invites extrapolation: the strategy
    // does NOT compound. Each cap comes from the daily-loss limit, not from
    // the balance, so profits do not grow position size. This is a
    // capital-efficiency ratio, not a growth rate.
    double allocated = 0;
    // Account-level equity, from TradeJournal::roi_basis. `from` is printed
    // because the baseline is chosen by a rule (see roi_from_sessions) and a
    // rule whose choice is invisible is a rule that can lie.
    bool roi_ok = false;
    std::string roi_from;
    double roi_baseline = 0;
    double roi_equity = 0;
    int roi_sessions = 0;
    bool mtd_ok = false;
    std::string mtd_from;
    double mtd_baseline = 0;
};

// Build a row from the engine's per-symbol live snapshot.
//
// A named function for one subtraction, because that subtraction is the single
// most likely thing in this whole feature to be silently wrong. LiveSnapshot's
// realized_pnl is GROSS: Portfolio::apply books (price - avg) * qty and charges
// the commission to cash only. On 2026-08-14 it reported -$597.01 against
// -$612.09 of actual cash on 13 fills, and on 2026-08-26 SNDQ's fees were 12.9%
// of its gross. A page promising "what the day banked" must not print the
// pre-commission figure, and inline at the call site there is nowhere for a test
// to catch it if it does.
inline EodSymbolRow eod_row_from(std::string symbol, double realized_gross,
                                 double fees, double qty, double avg_price,
                                 double unrealized, double peak_notional = 0,
                                 double max_notional = 0) {
    EodSymbolRow r;
    r.symbol = std::move(symbol);
    r.realized_net = realized_gross - fees;
    r.fees = fees;
    r.qty = qty;
    r.avg_price = avg_price;
    r.unrealized = unrealized;
    r.peak_notional = peak_notional;
    r.max_notional = max_notional;
    return r;
}

// Sum helpers, exposed so the caller and the tests agree on the arithmetic.
inline double eod_realized_net(const std::vector<EodSymbolRow>& rows) {
    double t = 0;
    for (const EodSymbolRow& r : rows) t += r.realized_net;
    return t;
}
inline double eod_fees(const std::vector<EodSymbolRow>& rows) {
    double t = 0;
    for (const EodSymbolRow& r : rows) t += r.fees;
    return t;
}
inline double eod_open_pnl(const std::vector<EodSymbolRow>& rows) {
    double t = 0;
    for (const EodSymbolRow& r : rows) t += r.unrealized;
    return t;
}
inline int eod_held_count(const std::vector<EodSymbolRow>& rows) {
    int n = 0;
    for (const EodSymbolRow& r : rows)
        if (r.qty != 0.0) ++n;
    return n;
}

// Peak capital deployed, summed across symbols. Exposed so the caller and the
// tests agree on the arithmetic, like the other eod_* helpers.
inline double eod_deployed_peak(const std::vector<EodSymbolRow>& rows) {
    double t = 0;
    for (const EodSymbolRow& r : rows) t += r.peak_notional;
    return t;
}

// Capital allocated: the sum of the live symbols' notional ceilings. See
// EodReport::allocated for why this and not the balance or the daily figure.
inline double eod_allocated(const std::vector<EodSymbolRow>& rows) {
    double t = 0;
    for (const EodSymbolRow& r : rows) t += r.max_notional;
    return t;
}

// The page body. Plain text, newline-separated, no trailing newline — ntfy
// renders it as-is and the log ring stores it a line at a time.
std::string format_eod_report(const EodReport& r);

} // namespace tt::ui
