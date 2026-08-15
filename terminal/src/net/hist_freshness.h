#pragma once

// Freshness of the historical bars we actually RECEIVED, per symbol.
//
// On 2026-08-07 the tws-data client started failing historical-bar requests on
// a fixed 30-minute cycle from ~10:31. SOXS, AAOX and SNDQ 5-minute candles
// went 4.5-5.2 hours stale (last good refresh 09:17 / 09:47 / 10:01) while
// their strategies kept trading, and nothing in /diag said a word. Three
// separate blind spots let it run all day:
//
//   1. data.connected stays true — the socket is half-open, not dead.
//   2. oldest_history_age_ms (net/hist_pacing.h) is keyed off the PENDING set,
//      and a dead request is cancelled at the 20s mark. Nothing ever ages
//      there, so any metric derived from in-flight requests is structurally
//      incapable of seeing this failure. It has to be measured from the
//      SUCCESSES instead — that is what this file does.
//   3. broker_upstream only covers the ORDERS client (tws_->upstream_connected).
//
// Deliberately source-agnostic: it hangs off the one on_candles success path in
// App, so it reports the same way whether the bars came from TwsData or
// GatewayData, and it cannot be fooled by a source that "answers" by cancelling.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tt::net {

// Timestamps handed to this class must come from a STEADY clock, for the same
// reason oldest_hist_ms does: a wall-clock/NTP jump around a gateway restart
// would otherwise show up as hours of fake staleness and page the phone.

// A traded symbol whose bars have gone past the grace period.
struct StaleBars {
    std::string symbol;
    // TRUE staleness, reported to the operator as-is, so a page still says "16h"
    // when it means 16h. For a never-answered symbol there is no such age, so it
    // carries the session's own age instead.
    int64_t age_ms = 0;
    bool ever = false;    // false = no bar set has EVER arrived for this symbol
    // The deliveries are ON TIME and the SERIES is not advancing: `age_ms` is
    // then the age of the newest BAR, not of the last delivery. A different
    // fault with a different fix — nothing is wrong with the request path, so
    // "check the tws-data retry lines" is the wrong advice for it — and the one
    // that ran five hours undetected on 2026-08-14 (STKH pinned at x10010).
    bool frozen = false;
};

// Bar length in ms for an interval string ("5m" / "1h" / "1d" — the optimizer's
// table, sweep_interval_str). 0 for anything unrecognised, which every caller
// reads as "do not judge the data's age".
inline int64_t hist_interval_ms(const std::string& interval) {
    if (interval == "5m") return 5 * 60 * 1000;
    if (interval == "1h") return 60 * 60 * 1000;
    if (interval == "1d") return 24 * 60 * 60 * 1000;
    return 0;
}

// A symbol the watchdog is entitled to expect refreshes for, together with the
// cadence that is supposed to deliver them.
//
// The cadence is per SYMBOL and not one figure for the lineup. 0.12.0 took a
// single grace off the slowest armed symbol, which lets one neighbour on a
// 240-minute re-optimize cadence stretch the threshold for everybody to 12
// hours: replay 2026-08-07 into that lineup and SOXS sits 284 minutes stale
// inside a 720-minute grace, so the outage this watchdog exists for pages
// nobody, all session, for the whole lineup.
struct WatchedSymbol {
    std::string symbol;
    double refresh_interval_min = 0;   // this symbol's own autopilot cadence
};

// Grace period before stale bars are worth paging about, from the refresh
// cadence that is supposed to keep them fresh.
//
// Live bars are only re-fetched by the autopilot's optimize cycle, which runs
// on a per-symbol timer (30 minutes in production) and is SERIALIZED across the
// lineup — pump_autopilot refuses to start a cycle while another tournament is
// running — so a symbol whose slot collides with a neighbour's tournament slips
// to the next pass. One slipped cycle is normal; two in a row is not. 3x the
// cadence (90 min at the production setting) therefore cannot fire on a healthy
// 30-minute rhythm, and on the 2026-08-07 outage it would have paged at 10:47
// off the 09:17 SOXS refresh — three hours before anyone noticed.
//
// The floor covers a lineup configured with a short autopilot interval: a
// tournament legitimately takes minutes, so 3 x 5 min would cry wolf.
inline constexpr int64_t kBarStaleGraceFloorMs = 45 * 60 * 1000;

// How recent a delivery has to be to count as EVIDENCE that the session is
// still serving history (HistoryFreshness::refreshing below).
//
// Deliberately absolute, and deliberately NOT each symbol's own grace period.
// The grace is 3x that symbol's cadence, so "inside its grace" stretches to 12
// hours for a symbol on a 240-minute re-optimize cycle — the same heterogeneous
// lineup this file already documents above. A symbol that has been silent for
// eleven of those hours is not proof of anything, and hist_stall_alert spends
// this evidence on an imperative verdict ("do NOT restart the gateway"). The
// floor grace is the shortest silence this watchdog is ever willing to call
// stale, which makes it the most generous bound that is still current.
inline constexpr int64_t kRefreshEvidenceMs = kBarStaleGraceFloorMs;

// (double because Autopilot::Sym::interval_min is one — the UI edits it.)
inline int64_t bar_stale_grace_ms(double refresh_interval_min) {
    const double slack = refresh_interval_min > 0
                             ? refresh_interval_min * 3.0 * 60'000.0
                             : 0.0;
    return slack > static_cast<double>(kBarStaleGraceFloorMs)
               ? static_cast<int64_t>(slack)
               : kBarStaleGraceFloorMs;
}

// The window this watchdog is entitled to judge a symbol against: the later of
// the live session's start and today's market open — i.e. the SHORTER of the
// two elapsed times. Both terms earn their place:
//
//   - session_ms alone was the rule until 0.21.0, and it pages an operator at
//     19:10 about candles that stopped because the market did (2026-08-11).
//   - rth_open_ms alone would page at 09:31 on a session that auto-started at
//     09:25, one minute in, before anything has had a chance to deliver.
//
// A one-line function so that the gating decision itself is a thing the tests
// can drive, rather than an expression buried in a 60-line pump. Pass
// tt::rth_open_elapsed_ms(localtime) for the second argument; it is 0 outside
// regular hours, which is what closes the gate.
inline int64_t hist_armed_ms(int64_t session_ms, int64_t rth_open_ms) {
    return rth_open_ms < session_ms ? rth_open_ms : session_ms;
}

// How long the watchdog must have been ARMED — market open AND session live —
// before it is entitled to judge any symbol. Below this, stale() reports
// nothing at all; at zero armed_ms (the market is shut) that IS the 2026-08-11
// gate, and just after the open it is the settle-in window.
//
// A FIXED settle-in, and that is the whole point of it. 0.21.0 spent the same
// window as a per-symbol cap — `overdue = min(age, armed_ms)` — which reads
// well and is wrong in a way that only arithmetic shows: armed_ms can never
// exceed the length of the regular session, measured at 389 min on a full day
// and 209 min on a 1pm half-day, so a symbol whose grace is longer than that
// could never be reported stale at any second of any trading day. The grace is
// 3x the autopilot cadence and the Trade panel allows a cadence up to 480 min
// (config.json is not clamped at all), so every symbol from 130 min up was
// silently unwatched — for 240 min, probed over all 23400 seconds of RTH with
// the symbol five days dead: zero pages, where the pre-0.21.0 rule paged at
// every one of them. Coverage did not even fail cleanly at a cliff; it thinned
// out first (a 100-min cadence kept 89 pageable minutes of the day, a 125-min
// one 14). Suppressing a real outage is a strictly worse failure than the false
// page 0.21.0 set out to fix, and a bound that does not scale with the grace
// cannot do it to any cadence.
//
// It is the grace floor because that is already the shortest silence this
// watchdog is ever willing to call stale.
inline constexpr int64_t kHistArmSettleMs = kBarStaleGraceFloorMs;

// The page text for a history stall, built where it can be tested rather than
// inline in App::pump_history_watchdog.
//
// It exists because the OLD text was actively misleading: it ended "(data
// socket reports CONNECTED - check IB Gateway)". On 2026-08-10 four symbols
// went hours stale while two others were served perfectly by the SAME socket,
// the same session and the same three farms, in the very cycles the others
// died — /diag read farms 3/3, gateway_authed true, and not one half-open
// reconnect all process. Sending the operator to restart the gateway on that
// evidence is worse than useless: a forced re-login can land in IBKR's
// maintenance window (the 2026-07-31 12-hour outage) and repeated failed logins
// lock the account. Naming the symbols that ARE refreshing puts the actual
// diagnosis in the page — the stall is app-side, not upstream.
//
// `detail` is the caller's per-symbol age list; `refreshing` is the symbols
// with POSITIVE evidence of a recent delivery (HistoryFreshness::refreshing).
//
// It is not "the watched symbols minus the stale ones". That set means "has not
// yet crossed its OWN grace period", which is not the same claim at all: a
// symbol that has never delivered a bar this session is aged from session start
// and a symbol on a 240-minute cadence has a 12-hour grace, so both sit outside
// `stale` for hours while delivering nothing — and the verdict below would then
// name a dead symbol as the reason to keep away from the gateway. That is the
// mirror image of the misdirection this text was written to remove: the old
// wording sent the operator to a healthy gateway, and that one would tell them
// to stay away from a broken one.
inline std::string hist_stall_alert(const std::string& detail,
                                    const std::vector<StaleBars>& stale,
                                    const std::vector<std::string>& refreshing,
                                    bool socket_connected) {
    std::string healthy;
    for (const std::string& s : refreshing) {
        if (!healthy.empty()) healthy += ", ";
        healthy += s;
    }
    // A FROZEN series needs different words, and the difference is the whole
    // point of measuring bar age separately. "check the tws-data 'retrying as
    // req' lines" sends the operator to look for failing requests; when the
    // requests are succeeding on time and the SERIES is not advancing there are
    // no such lines to find, and the reader concludes the page is spurious.
    // Naming the fault is what makes it actionable.
    bool any_frozen = false;
    for (const StaleBars& s : stale) any_frozen = any_frozen || s.frozen;
    if (any_frozen) {
        std::string frozen;
        for (const StaleBars& s : stale) {
            if (!s.frozen) continue;
            if (!frozen.empty()) frozen += ", ";
            frozen += s.symbol;
        }
        return "WATCHDOG historical bars have STOPPED ADVANCING for " +
               std::to_string(stale.size()) +
               " traded symbol(s) - strategies are trading on stale candles: " +
               detail + " (the fetches are SUCCEEDING on time for " + frozen +
               " and returning the same newest bar - this is a frozen SERIES, "
               "not a dead request path, so there are no 'retrying as req' lines "
               "to find; do NOT restart the gateway)";
    }
    // Disconnected is the ordinary outage the reconnect path is already working
    // on. Connected with NOTHING refreshing is the one case where the upstream
    // is still a live suspect, so it does not get the "not the gateway" verdict
    // — but it still does not get told to restart anything blind.
    const std::string state =
        !socket_connected
            ? " (data socket disconnected - the reconnect path is already on it)"
        : healthy.empty()
            ? " (data socket CONNECTED but NO watched symbol is refreshing - "
              "check the tws-data log for 'retrying as req' / 'reconnecting data "
              "session' lines before touching the gateway)"
            : " (data socket CONNECTED and still refreshing " + healthy +
              " on the same session - app-side history stall, not the gateway: "
              "check the tws-data 'retrying as req' lines; do NOT restart the "
              "gateway)";
    return "WATCHDOG historical bars have stopped refreshing for " +
           std::to_string(stale.size()) +
           " traded symbol(s) - strategies are trading on stale candles: " +
           detail + state;
}

// TWO AGES, AND ONLY ONE OF THEM IS ABOUT THE DATA.
//
// This class recorded steady_ms() at delivery and called it freshness, so
// `age_ms` has always meant "time since a batch ARRIVED". A source that keeps
// answering, on time, with a series that has stopped advancing therefore reads
// perfectly fresh — and that is not a hypothetical:
//
//   2026-08-14. STKH's 5-minute series was pinned at exactly x10010 bars from
//   10:44:49 through 15:44:49 — five hours — while MUU's grew the expected six
//   bars per half hour (x9681 -> x9747). After the close STKH's count began to
//   SHRINK (x10007, x10001, x9997) as the 6-month window slid off the back with
//   nothing being appended at the front. The watchdog reported "still refreshing
//   MUU, SNDQ, STKH, SNDU" the whole time, and /diag published the delivery age
//   under the field name `last_bar_age_ms`, which is a claim about a bar.
//
// The 2026-08-07 outage this file was written for was the OTHER failure —
// requests that stopped being answered at all — and the delivery age catches
// that one perfectly. Both are real, so both are measured, separately:
//
//   - DELIVERY age (age_ms): is the request path alive? Diagnostic; also what
//     `refreshing()` spends as evidence for "do NOT restart the gateway".
//   - BAR age (bar_age_ms): is the DATA advancing? This is what the watchdog
//     judges on, because "strategies are trading on stale candles" is a
//     statement about candles.
//
// A symbol whose newest bar stops moving is stale even while its deliveries are
// punctual, which is the case that ran for five hours in silence.
class HistoryFreshness {
public:
    // Data worker thread, once per successfully delivered bar set.
    //
    // `newest_bar_ms` is the epoch-ms timestamp of the last bar in the batch, or
    // 0 when the caller has none to give (an empty batch, or a call site that
    // has not been taught yet) — in which case only the delivery age is
    // recorded and the bar age stays at whatever the last real batch left, so a
    // caller who cannot answer cannot accidentally certify the data as fresh.
    //
    // WALL-CLOCK ms for the bar, STEADY ms for the delivery, deliberately: a bar
    // timestamp comes from the exchange and can only be compared against wall
    // time, while the delivery age must survive an NTP step around a gateway
    // restart (see the note at the top of this file).
    void record(const std::string& symbol, const std::string& interval,
                int64_t now_ms, int64_t newest_bar_ms = 0) {
        std::lock_guard<std::mutex> g(mu_);
        const std::string k = key(symbol, interval);
        last_[k] = now_ms;
        if (newest_bar_ms > 0) newest_bar_[k] = newest_bar_ms;
    }

    // ms between `wall_now_ms` and the newest bar this symbol has ever
    // delivered, or -1 if no batch with a usable timestamp has arrived.
    //
    // Never negative: a bar's timestamp is its OPEN, and a source that hands
    // over the in-progress bar of the current interval legitimately produces a
    // stamp in the future relative to nothing in particular. Clamping at 0 keeps
    // "the data is current" as the answer rather than a negative age that every
    // `>` threshold silently passes.
    int64_t bar_age_ms(const std::string& symbol, const std::string& interval,
                       int64_t wall_now_ms) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = newest_bar_.find(key(symbol, interval));
        if (it == newest_bar_.end()) return -1;
        const int64_t age = wall_now_ms - it->second;
        return age > 0 ? age : 0;
    }

    // ms since the last successful delivery, or -1 if none has ever arrived in
    // this process. Keyed by interval as well as symbol on purpose: the daily
    // lineup builder fetches "1d" bars for every candidate, and a symbol-only
    // key would let that mark a symbol fresh while its 5m series — the one the
    // engine seeds and the optimizer scores — sat five hours old.
    int64_t age_ms(const std::string& symbol, const std::string& interval,
                   int64_t now_ms) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = last_.find(key(symbol, interval));
        return it == last_.end() ? -1 : now_ms - it->second;
    }

    // Largest age across `symbols`, for the single top-level /diag figure.
    // Never-answered symbols are skipped (they have no age); -1 = nobody has
    // one. The watchdog uses stale() below, which does not skip them.
    int64_t worst_age_ms(const std::vector<std::string>& symbols,
                         const std::string& interval, int64_t now_ms) const {
        std::lock_guard<std::mutex> g(mu_);
        int64_t worst = -1;
        for (const std::string& s : symbols) {
            const auto it = last_.find(key(s, interval));
            if (it == last_.end()) continue;
            const int64_t age = now_ms - it->second;
            if (age > worst) worst = age;
        }
        return worst;
    }

    // Symbols past their OWN grace period, worst first. A symbol that has NEVER
    // been answered is not healthy — the failure can just as easily start before
    // a symbol's first refresh lands — so it is aged from `session_ms`.
    //
    // TWO separate questions, deliberately kept apart after 0.21.0 answered
    // them with one number and lost a symbol class doing it (kHistArmSettleMs):
    //
    //   1. May this watchdog judge at all right now? `armed_ms` — the time
    //      since the LATER of the live session's start and today's market open
    //      (App::pump_history_watchdog, tt::rth_open_elapsed_ms) — has to clear
    //      a FIXED settle-in. Outside regular hours the caller passes 0 and
    //      nothing is reported however old it is: the 2026-08-11 gate.
    //   2. Is this particular symbol overdue? Its own true age against its own
    //      grace, uncapped. A cap keyed to the arming window is what made a
    //      long-cadence symbol permanently unpageable.
    //
    // The settle-in is what stops the open boundary paging on evidence the
    // market itself explains: a session left running overnight still holds
    // yesterday's deliveries, so at 09:30:01 a symbol last served at 17:00 is
    // sixteen hours old. It does NOT forgive those sixteen hours indefinitely,
    // and it must not: pump_autopilot keeps cycling all night (it disarms only
    // on !live_running), so in a left-running session those fetches were being
    // made and were failing. That is the 2026-08-07 stall, and after 45 minutes
    // of open market it gets said out loud.
    //
    // /diag's worst_age_ms and age_ms are untouched by any of this: the alert is
    // gated, the measurement is not.
    // THE THIRD question, added 2026-08-14 and the reason this signature grew a
    // wall clock: is the DATA advancing? A symbol whose deliveries are punctual
    // and whose newest bar has not moved in five hours is exactly as blind as
    // one nobody is answering, and the pre-existing measurement — time since a
    // batch arrived — cannot express it. See the class comment.
    //
    // The bar-age threshold is the LARGER of the symbol's cadence grace and
    // three bar intervals, never the cadence grace alone: on a "1d" series the
    // newest bar is legitimately hours or a weekend old, so the 45-minute floor
    // that is right for 5-minute bars would page every single morning. An
    // interval hist_interval_ms does not recognise is not judged on data age at
    // all — inventing a bound would be worse than the blind spot.
    std::vector<StaleBars> stale(const std::vector<WatchedSymbol>& watched,
                                 const std::string& interval, int64_t now_ms,
                                 int64_t wall_now_ms, int64_t session_ms,
                                 int64_t armed_ms) const {
        std::vector<StaleBars> out;
        if (armed_ms <= kHistArmSettleMs) return out;   // shut, or too early to judge
        const int64_t ivl_ms = hist_interval_ms(interval);
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const WatchedSymbol& w : watched) {
                const std::string k = key(w.symbol, interval);
                const auto it = last_.find(k);
                const bool ever = it != last_.end();
                const int64_t age = ever ? now_ms - it->second : session_ms;
                const int64_t grace = bar_stale_grace_ms(w.refresh_interval_min);
                if (age > grace) {
                    out.push_back({w.symbol, age, ever, false});
                    continue;   // nothing is arriving: the older fault wins the page
                }
                if (ivl_ms <= 0) continue;              // interval we can't judge
                const auto bt = newest_bar_.find(k);
                if (bt == newest_bar_.end()) continue;  // no bar stamp: nothing to judge
                const int64_t raw = wall_now_ms - bt->second;
                const int64_t bar_age = raw > 0 ? raw : 0;
                const int64_t bar_grace = grace > 3 * ivl_ms ? grace : 3 * ivl_ms;
                if (bar_age > bar_grace)
                    out.push_back({w.symbol, bar_age, true, true});
            }
        }
        for (size_t i = 1; i < out.size(); ++i)   // tiny n (a lineup is <10)
            for (size_t j = i; j > 0 && out[j].age_ms > out[j - 1].age_ms; --j)
                std::swap(out[j], out[j - 1]);
        return out;
    }

    // Watched symbols with a delivery of their OWN inside kRefreshEvidenceMs:
    // the only symbols entitled to back hist_stall_alert's "still refreshing X
    // on the same session" verdict. A symbol that has never been answered is
    // absent from the map and so can never appear here, which is the case
    // "watched minus stale" got wrong — see kRefreshEvidenceMs.
    std::vector<std::string> refreshing(const std::vector<WatchedSymbol>& watched,
                                        const std::string& interval,
                                        int64_t now_ms) const {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> g(mu_);
        for (const WatchedSymbol& w : watched) {
            const auto it = last_.find(key(w.symbol, interval));
            if (it == last_.end()) continue;                    // never delivered
            if (now_ms - it->second > kRefreshEvidenceMs) continue;
            out.push_back(w.symbol);
        }
        return out;
    }

    // Forget every delivery. Freshness belongs to a live SESSION, not to the
    // process: a delivery carried over from a session that has since stopped
    // reads as hours of staleness on the first frame of the next one, and the
    // terminal is meant to stay up across the nightly 15:55 stop / 09:25 start,
    // so that is the ordinary case rather than a corner.
    //
    // The arming settle-in now also holds that carry-over off the phone for the
    // first 45 minutes of the open, but this is still the thing that keeps the
    // numbers honest: /diag's worst_age_ms and age_ms are ungated, and refreshing() would
    // otherwise let a delivery from a dead session stand as evidence that this
    // one is being served. Called every idle frame, so it must stay cheap on an
    // already-empty map.
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        last_.clear();
        newest_bar_.clear();
    }

private:
    static std::string key(const std::string& symbol, const std::string& interval) {
        return symbol + '\x1f' + interval;   // unit separator: not a ticker char
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, int64_t> last_;        // steady ms, at DELIVERY
    std::unordered_map<std::string, int64_t> newest_bar_;  // wall ms, the BAR's own stamp
};

} // namespace tt::net
