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

// (double because Autopilot::Sym::interval_min is one — the UI edits it.)
inline int64_t bar_stale_grace_ms(double refresh_interval_min) {
    const double slack = refresh_interval_min > 0
                             ? refresh_interval_min * 3.0 * 60'000.0
                             : 0.0;
    return slack > static_cast<double>(kBarStaleGraceFloorMs)
               ? static_cast<int64_t>(slack)
               : kBarStaleGraceFloorMs;
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

    // Symbols past `grace_ms`, worst first. A symbol that has NEVER been
    // answered is not healthy — the failure can just as easily start before a
    // symbol's first refresh lands — so it is aged from `session_ms`, how long
    // the live session has been running. That is also what makes the grace
    // period double as a settle-in window at session start.
    std::vector<StaleBars> stale(const std::vector<std::string>& symbols,
                                 const std::string& interval, int64_t now_ms,
                                 int64_t grace_ms, int64_t session_ms) const {
        std::vector<StaleBars> out;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const std::string& s : symbols) {
                const auto it = last_.find(key(s, interval));
                const bool ever = it != last_.end();
                const int64_t age = ever ? now_ms - it->second : session_ms;
                if (age > grace_ms) out.push_back({s, age, ever});
            }
        }
        for (size_t i = 1; i < out.size(); ++i)   // tiny n (a lineup is <10)
            for (size_t j = i; j > 0 && out[j].age_ms > out[j - 1].age_ms; --j)
                std::swap(out[j], out[j - 1]);
        return out;
    }

private:
    static std::string key(const std::string& symbol, const std::string& interval) {
        return symbol + '\x1f' + interval;   // unit separator: not a ticker char
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, int64_t> last_;
};

} // namespace tt::net
