#include "eod_report.h"

#include <cstdarg>
#include <cstdio>

namespace tt::ui {

namespace {

// snprintf into a stack buffer and append. Every line here is bounded by a
// handful of fixed-width numbers plus one symbol, so 256 is not a guess.
void addf(std::string& out, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

void addf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, static_cast<size_t>(n) < sizeof buf
                                   ? static_cast<size_t>(n)
                                   : sizeof buf - 1);
}

const char* plural(int n) { return n == 1 ? "" : "s"; }

// Thousands separators, because the capital figures are five and seven digits
// and "1002794.09" against "27917.00" is two numbers nobody can compare at a
// glance on a phone. Hand-rolled: std::locale on MinGW does not reliably carry
// a grouping facet, and a page that silently stopped grouping would be worse
// than one that never did.
std::string money(double v) {
    char raw[48];
    const int n = std::snprintf(raw, sizeof raw, "%.0f", v < 0 ? -v : v);
    if (n <= 0) return "0";
    std::string digits(raw, static_cast<size_t>(n));
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    if (v < 0) out.push_back('-');
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

} // namespace

std::string format_eod_report(const EodReport& r) {
    std::string out;
    addf(out, "%s %s", kEodReportTag, r.date.c_str());

    // The halt is the headline when it happened. A day that hit its loss limit
    // is not a day you read the per-symbol table of first.
    if (r.halted) addf(out, "\nHALTED - the session stopped itself on a risk limit.");

    if (!r.journal_available) {
        // NOT "no sessions". The day's totals live in journal.db and it did not
        // open, so they are unknown — reporting the unknown as a zero would turn
        // a storage fault into a false all-clear.
        addf(out, "\n\nnet unavailable - journal.db is not open, so today's "
                  "session and fill totals could not be read.");
    } else if (r.sessions == 0 && r.symbols.empty()) {
        // Not padding. This report only fires on a day the market actually
        // opened (session_should_stop returns false otherwise), so reaching here
        // means a trading day passed with no live session at all — 2026-08-21
        // was exactly that and nothing announced it.
        addf(out, "\n\nNO SESSION RAN TODAY. The market opened and nothing traded.");
        return out;
    } else if (r.sessions == 0) {
        // A session was demonstrably live at the close — the per-symbol rows
        // below came out of it — and yet the journal has no row for today. That
        // is a journalling fault, and journal.db is the authoritative trade
        // record, so it is said out loud rather than shown as a zero.
        addf(out, "\n\nnet unavailable - a session was running but journal.db "
                  "has no row for today.");
    } else {
        addf(out, "\n\nnet %+.2f on %d fill%s, %d session%s", r.net, r.fills,
             plural(r.fills), r.sessions, plural(r.sessions));
        if (r.session_still_open)
            addf(out,
                 "\n(a session was still running when this was sent, so `net` "
                 "does not include it yet)");
    }

    if (!r.symbols.empty()) {
        const double realized = eod_realized_net(r.symbols);
        const double open_pnl = eod_open_pnl(r.symbols);
        const int held = eod_held_count(r.symbols);
        addf(out, "\n\n  realized %+10.2f  (%.2f in fees)", realized,
             eod_fees(r.symbols));
        addf(out, "\n  open     %+10.2f  (%d position%s held overnight)", open_pnl,
             held, plural(held));
        // realized + open only reconciles to `net` for a single-session day; say
        // so rather than let two numbers that do not add up sit next to each other.
        if (r.sessions > 1)
            addf(out,
                 "\n  (last session only - %d ran today, `net` covers them all)",
                 r.sessions);

        std::string flat;
        addf(out, "\n");   // the totals above, the per-symbol table below
        for (const EodSymbolRow& s : r.symbols) {
            const bool traded = s.realized_net != 0.0 || s.fees != 0.0;
            if (!traded && s.qty == 0.0) {
                if (!flat.empty()) flat += ", ";
                flat += s.symbol;
                continue;
            }
            addf(out, "\n  %-5s %+9.2f realized", s.symbol.c_str(), s.realized_net);
            if (s.qty != 0.0)
                addf(out, " | %.0f @ %.2f, %+.2f open", s.qty, s.avg_price,
                     s.unrealized);
            else
                addf(out, " | flat");
        }
        if (!flat.empty()) addf(out, "\n  no trades: %s", flat.c_str());
    }

    // ---- return on capital ------------------------------------------------
    // Both denominators, both labelled. See the note on EodReport: the same
    // figure is +0.79% of this account or ~+26% of what the strategy commits,
    // and a page that printed only one of those would be either dispiriting or
    // flattering rather than informative.
    if (r.deployed_peak > 0.0 || r.roi_ok) {
        addf(out, "\n\nreturn");
        if (r.deployed_peak > 0.0) {
            // Today, against capital that was actually at work. The guard is
            // deployed_peak > 0 and not >= 0: a zero denominator means NOT
            // MEASURED, and dividing by it would print an infinity.
            addf(out, "\n  today    %+10.2f  on %s deployed  = %+.2f%%",
                 r.net, money(r.deployed_peak).c_str(),
                 100.0 * r.net / r.deployed_peak);
        }
        if (r.mtd_ok && r.mtd_baseline > 0.0) {
            const double chg = r.roi_equity - r.mtd_baseline;
            addf(out, "\n  month    %+10.2f  = %+.2f%% of account  (from %s)", chg,
                 100.0 * chg / r.mtd_baseline, r.mtd_from.c_str());
        }
        if (r.roi_ok && r.roi_baseline > 0.0) {
            const double chg = r.roi_equity - r.roi_baseline;
            addf(out,
                 "\n  overall  %+10.2f  = %+.2f%% of account  (from %s, %d session%s)",
                 chg, 100.0 * chg / r.roi_baseline, r.roi_from.c_str(),
                 r.roi_sessions, plural(r.roi_sessions));
            addf(out, "\n  equity   %s", money(r.roi_equity).c_str());
        } else if (r.deployed_peak > 0.0) {
            // Said out loud rather than omitted: a missing line reads as zero.
            addf(out, "\n  overall  not available - no believable equity baseline yet");
        }
    }

    if (r.daily_loss_limit > 0.0) {
        // daily_loss is positive when DOWN on the day (see LiveSnapshot::risk),
        // so a profitable day reports 0 used rather than a negative fraction.
        const double used = r.daily_loss > 0.0 ? r.daily_loss : 0.0;
        addf(out, "\n\nrisk: %.2f of the %.0f daily-loss limit (%.0f%%)", used,
             r.daily_loss_limit, 100.0 * used / r.daily_loss_limit);
    }
    return out;
}

} // namespace tt::ui
