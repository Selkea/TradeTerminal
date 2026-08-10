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
    int64_t age_ms = 0;   // effective staleness (see stale() for never-answered)
    bool ever = false;    // false = no bar set has EVER arrived for this symbol
};

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

class HistoryFreshness {
public:
    // Data worker thread, once per successfully delivered bar set.
    void record(const std::string& symbol, const std::string& interval,
                int64_t now_ms) {
        std::lock_guard<std::mutex> g(mu_);
        last_[key(symbol, interval)] = now_ms;
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
    // a symbol's first refresh lands — so it is aged from `session_ms`, how long
    // the live session has been running. That is also what makes the grace
    // period double as a settle-in window at session start.
    std::vector<StaleBars> stale(const std::vector<WatchedSymbol>& watched,
                                 const std::string& interval, int64_t now_ms,
                                 int64_t session_ms) const {
        std::vector<StaleBars> out;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const WatchedSymbol& w : watched) {
                const auto it = last_.find(key(w.symbol, interval));
                const bool ever = it != last_.end();
                const int64_t age = ever ? now_ms - it->second : session_ms;
                if (age > bar_stale_grace_ms(w.refresh_interval_min))
                    out.push_back({w.symbol, age, ever});
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
    // process: stale() ages an answered symbol absolutely, and the settle-in
    // window covers only symbols that have never been answered, so a delivery
    // carried over from a session that has since stopped reads as hours of
    // staleness on the first frame of the next one. The terminal is meant to
    // stay up across the nightly 15:55 stop / 09:25 start, so that is the
    // ordinary case rather than a corner. Called every idle frame, so it must
    // stay cheap on an already-empty map.
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        last_.clear();
    }

private:
    static std::string key(const std::string& symbol, const std::string& interval) {
        return symbol + '\x1f' + interval;   // unit separator: not a ticker char
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, int64_t> last_;
};

} // namespace tt::net
