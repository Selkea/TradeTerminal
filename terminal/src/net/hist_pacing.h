#pragma once

// Pacing rules for IB historical-data requests, split out of tws_data.cpp so
// they can be tested without a gateway.
//
// Measured on the live deployment over 178 requests (2026-08-03):
//   103 successful 6M/5m fetches: min 0s, median 6s, p90 15s, MAX 17s
//    75 killed fetches:           all at exactly 20s, ZERO bars, no error
// The distribution is bimodal with a 3-second gap, so the 20s timeout is not
// too short — the failures are never answered at all, at any latency. Raising
// it recovers nothing and only delays recovery.
//
// One rule predicted 177 of those 178 outcomes:
//   a reqHistoricalData issued too soon after a historicalDataEnd that
//   delivered >= ~3000 bars is never answered.
// Grouped by the gap since the previous delivery:
//   gap > 20s                 -> alive 85, dead  0
//   gap <= 5s, prev < 3000    -> alive 16, dead  0
//   gap <= 5s, prev 3000-6000 -> alive  0, dead 12
//   gap <= 5s, prev > 6000    -> alive  1, dead 64
// It is the size of the PRECEDING response that matters, not the size of the
// request being sent and not the send-to-send spacing: a request sent at a 4s
// gap after a 2,163-bar batch survived, while one at a 9s gap after a 7,700-bar
// batch died. No IBKR pacing-violation error (162/165/366/420) is ever emitted —
// IB answers a breach with silence, which is exactly what we observe.
//
// ---- 2026-08-13: THE HOLE IN THE TABLE ABOVE, AND WHAT FELL INTO IT --------
//
// READ THE BUCKETS AGAIN BEFORE TRUSTING THEM. 85+16+12+64 = 177 observations,
// and every one is either <= 5s or > 20s. THE 5-20s BAND CONTAINS NO DATA AT
// ALL. The original release nonetheless set the quiet window to 10s, reasoning
// "the danger zone measured out at ~5s; 10s is comfortably clear of it" — i.e.
// the constant was INTERPOLATED INTO A GAP IN THE MEASUREMENT. The only gap the
// data ever showed safe is > 20s; 10s was never observed to be anything.
//
// Production filled the hole, and 10s is inside the danger zone:
//   [13:57:52] candles: SNXX 5m x9570      <- 9,570-bar delivery
//   [13:58:03] MUU 5m 6mo req 139 sent     <- 11s later, PAST the 10s window
//   [13:58:23] MUU 5m: history unanswered for 20s - cancelled req 139
// Reproduced ~20 times on 2026-08-13 alone, on a fixed 30-minute rebuild cycle.
// SOXS and RAM survived every cycle only because they are issued FIRST after a
// reconnect, onto a clean session; MUU and KORU always landed in the shadow of
// the big delivery and received ZERO bars for the whole session, leaving two
// live strategies trading on candles up to 120 minutes stale. Nothing is wrong
// with those contracts — KORU has itself delivered 5m x9725 on other days.
//
// The band is therefore now:
//   gap 11-12s, prev ~9,500-10,100 -> alive  0, dead ~20   (2026-08-13)
// Known-dead now reaches 12s and known-safe still starts at 20s. The window
// below is set ABOVE the measured-safe boundary, not between the two: picking
// anything in 12-20s would repeat the exact mistake this note exists to record.
//
// SECOND DEFECT, same incident: a SMALL delivery used to mask a BIG one. The
// gate looked only at the most recent historicalDataEnd, and tws_data.cpp
// stamped it on every delivery regardless of size:
//   [15:23:21] candles: SOXS 5m x10055   <- the poison
//   [15:23:32] candles: RAM  5m x2799    <- 2,799 < kBigBatchBars: "not big"
//   [15:23:33] MUU 5m 6mo req 32 sent    <- gate saw last=2,799 at 1s -> DEAD
// RAM's harmless batch cleared SOXS's poison 12 seconds early. The gate now
// remembers the last BIG delivery specifically, so nothing can reset it early.
// Fixing either defect alone leaves the wedge intact: a longer window still
// lets RAM mask SOXS, and tracking the big delivery still permits a send at 11s.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace tt::net {

// ---- request identity ------------------------------------------------------
//
// What makes two historical requests "the same request" is what goes ON THE
// WIRE: contract + endDateTime + durationString + barSizeSetting + whatToShow +
// useRTH. TwsData sends ""/"TRADES"/useRTH=1 for every historical fetch, so the
// identity is exactly (symbol, bar size, duration).
//
// It is NOT the caller's (interval, range). max_dur_idx clamps a range that
// exceeds what IB will serve for a bar size, so two different caller ranges
// collapse onto one duration: 1h/"5y" and 1h/"max" both send dur="2 Y",
// bar="1 hour". Keying the pacing gate on the caller's label let those two go
// out kHistMinGapMs apart as BYTE-IDENTICAL reqHistoricalData calls — exactly
// the violation the gate exists to prevent, reintroduced by the gate itself.
// net/bar_cache.h keys on the same resolved pair for the mirror-image reason:
// two requests IB cannot tell apart return the same series, so the second one
// need not be sent at all rather than merely delayed.
//
// These live here, rather than in tws_data.cpp where they used to, so a test can
// pin that the collapse happens — the defect was one call site passing the
// unclamped range, which nothing could see from outside.

inline const char* ib_bar_size(const std::string& interval) {
    if (interval == "1s") return "1 secs";
    if (interval == "1m") return "1 min";
    if (interval == "2m") return "2 mins";
    if (interval == "5m") return "5 mins";
    if (interval == "15m") return "15 mins";
    if (interval == "30m") return "30 mins";
    if (interval == "1h") return "1 hour";
    if (interval == "1d") return "1 day";
    return nullptr;
}

// Duration ladder, smallest to largest; index doubles as a comparable rank.
struct IbDur {
    const char* range;
    const char* dur;
};
inline constexpr IbDur kDurs[] = {
    {"1m", "60 S"},   {"5m", "300 S"}, {"15m", "900 S"}, {"30m", "1800 S"},
    {"1h", "3600 S"}, {"1d", "1 D"},   {"5d", "5 D"},    {"1mo", "1 M"},
    {"6mo", "6 M"},   {"1y", "1 Y"},   {"2y", "2 Y"},    {"5y", "5 Y"},
    {"max", "15 Y"}};
inline constexpr int kNumDurs = static_cast<int>(sizeof kDurs / sizeof kDurs[0]);

inline int dur_idx(const std::string& range) {
    for (int i = 0; i < kNumDurs; ++i)
        if (range == kDurs[i].range) return i;
    return -1;
}

// IB caps how much history one request may span per bar size; clamp so a
// "1m x 5y" chart still shows the most recent stretch instead of erroring.
inline int max_dur_idx(const std::string& interval) {
    if (interval == "1s") return dur_idx("30m");           // ~1800 1-sec bars
    if (interval == "1m" || interval == "2m") return dur_idx("1mo");
    if (interval == "5m" || interval == "15m" || interval == "30m")
        return dur_idx("6mo");
    if (interval == "1h") return dur_idx("2y");
    return kNumDurs - 1;                                   // daily: anything
}

// A caller's (interval, range) in IB's own terms.
struct HistWire {
    const char* bar = nullptr;   // barSizeSetting; nullptr = interval unknown
    const char* dur = nullptr;   // durationString, ALREADY clamped
    bool clamped = false;        // the caller's range did not survive the clamp
    bool ok() const { return bar && dur; }
};

inline HistWire hist_wire(const std::string& interval, const std::string& range) {
    HistWire w;
    w.bar = ib_bar_size(interval);
    const int di = dur_idx(range);
    if (!w.bar || di < 0) {
        w.bar = nullptr;
        return w;
    }
    const int cap = max_dur_idx(interval);
    w.clamped = di > cap;
    w.dur = kDurs[w.clamped ? cap : di].dur;
    return w;
}

// The identity key, for BOTH the pacing gate and the bar cache. One function so
// the two can never disagree about what "the same request" means.
// '\x1f' (unit separator) appears in no ticker, bar size or duration string.
inline std::string hist_key(const std::string& symbol, const std::string& bar_size,
                            const std::string& duration) {
    return symbol + '\x1f' + bar_size + '\x1f' + duration;
}
inline std::string hist_key(const std::string& symbol, const HistWire& w) {
    return hist_key(symbol, w.bar ? w.bar : "", w.dur ? w.dur : "");
}

// A delivery at or above this many bars poisons the next request for a while.
inline constexpr size_t kBigBatchBars = 3'000;
// Unanswered for this long = dead. See the note above: this is a classifier,
// not a patience setting.
inline constexpr int64_t kHistTimeoutMs = 20'000;
// Quiet window after a BIG delivery. 22s, not the 10s that shipped first:
// known-dead now reaches 12s and the only measured-safe gap is > 20s, so this
// sits ABOVE the safe boundary rather than inside the unmeasured 12-20s band.
//
// It deliberately EXCEEDS kHistTimeoutMs, which the original invariant forbade
// ("a blocked send must cost an extra loop pass, never a failed fetch"). That
// invariant conflated two holds that age differently:
//   - A FIRST send held here sits on io_loop's `deferred` queue and has not been
//     handed to HistRequests at all, so it has no age, cannot be classified
//     dead however long it waits, and is bounded instead by kHistQueueMaxWaitMs.
//   - A RETRY held here is for a request already on the wire and already dead,
//     so waiting DOES push it toward escalation — which is why kHistEscalateMs
//     below is now expressed in terms of this window instead of assuming it is
//     shorter than one timeout.
inline constexpr int64_t kBigBatchQuietMs = 22'000;

// Should we hold a history request back right now? `ms_since_big_end` is the age
// of the last BIG delivery specifically (>= kBigBatchBars), NOT of the last
// delivery of any size — that distinction is the whole of defect 2 above, where
// a 2,799-bar batch cleared a 10,055-bar batch's poison twelve seconds early.
// A session that has delivered no big batch at all passes a large sentinel.
inline bool history_send_blocked(int64_t ms_since_big_end) {
    return ms_since_big_end < kBigBatchQuietMs;
}

// Does this delivery re-arm the quiet window? Split out of tws_data.cpp so the
// masking rule is testable: the defect was not the threshold but the fact that
// EVERY delivery re-stamped the clock, which nothing outside that one line
// could observe. A delivery too small to poison must leave the clock alone.
inline bool history_delivery_poisons(size_t bars) { return bars >= kBigBatchBars; }

// A request unanswered for THIS long is escalated whether or not a retry was
// ever issued. Two timeouts PLUS one quiet window: exactly the age a request
// reaches when it dies, waits out the longest legitimate hold before its retry
// may go out, and then dies again — the natural escalation timeline.
//
// The quiet-window term is not padding. Once kBigBatchQuietMs exceeded one
// timeout, a bare 2 * kHistTimeoutMs would have escalated every request whose
// retry was still legitimately waiting for the window to pass, tearing down a
// healthy data session on a schedule. The reverse error is the one this project
// has already shipped: hist_pacing's rule is that the gate may delay a retry but
// must NEVER delay the recovery, so this must exceed the longest legal hold and
// nothing more.
//
// WHY IT IS AGE-BASED AND NOT PURELY RETRY-BASED. `already_retried` is set by
// HistRequests::begin_retry, and Io::retry_dead_history has to ask the send gate
// below before it may re-issue. A SendHold::Budget hold lasts until sends age
// out of a ten-minute window, so a retry held there left the request classified
// Retry forever: `retried` never got set, has_twice_dead() never fired, and
// io_loop's data-session reconnect — the recovery 0.15.1 restored, which had
// cleared this exact half-open state 286 times — became unreachable again. It
// became unreachable in precisely the situation it exists for, because the
// thing that spends the budget IS the degraded build. The gate may delay a
// retry; it must never delay the recovery.
//
// This cannot fire spuriously: a request only has an age here if it was
// genuinely put on the wire (pump_requests adds to HistRequests only on the send
// path), so 62 s of silence is 62 s of real silence. That same property is what
// makes the quiet window safe to lengthen past a timeout — a request still
// sitting on the deferred queue has no age here at all.
inline constexpr int64_t kHistEscalateMs = 2 * kHistTimeoutMs + kBigBatchQuietMs;

// What to do with an in-flight request of this age.
enum class HistAction {
    Wait,       // still within its normal turnaround
    Retry,      // dead, but never retried: cancel + re-issue on the same session
    Escalate,   // dead again, or dead too long to still be waiting on a retry
};

inline HistAction history_action(int64_t age_ms, bool already_retried) {
    if (age_ms <= kHistTimeoutMs) return HistAction::Wait;
    if (already_retried) return HistAction::Escalate;
    return age_ms > kHistEscalateMs ? HistAction::Escalate : HistAction::Retry;
}

// ---- send rate ------------------------------------------------------------
//
// The rule above is about ONE request following ONE big delivery. This part is
// about the aggregate, which the 2026-08-10 lineup build broke in two separate
// ways.
//
//  1. THE BURST. The build opens with 30 'candles: SYM 1d x21' deliveries all
//     stamped in the SAME SECOND (09:35:02) — the pool-ranking pass fires one
//     reqHistoricalData per scan hit in a single pump_requests loop, alongside
//     the scanner subscription. About 60 historical requests went out inside the
//     window, against IB's documented budget of 60 per 10 minutes. The data
//     session then went silent at 09:40:41 and stayed silent for 20 minutes;
//     a FRESH client at 10:00:43 was served instantly, so the budget, not the
//     gateway, was what had been spent.
//
//  2. THE IDENTICAL RE-ISSUE. SSPC 5m/6mo was requested four times, two of them
//     12 seconds apart. IB documents "making identical historical data requests
//     within 15 seconds" as a pacing violation in its own right. net/bar_cache.h
//     removes almost all of these by answering the second request from the first
//     one's delivery — but a cache can only answer AFTER a delivery, and the
//     duplicate that hurts is the one issued while the original is still in
//     flight. That one has to be stopped here.
//
// THE RATE. IB's ceiling is 60 requests in any rolling 10 minutes. Pacing at
// the sustainable average (one per 10 s) would make the build's own 36 requests
// take six minutes of pure spacing, which is slower than the target for the
// whole build — the ceiling is a WINDOW, not a metronome, and a burst that fits
// inside it is legal. So: enforce the window honestly, and add only enough
// minimum spacing to stop 30 requests landing in the same second.
inline constexpr int64_t kHistWindowMs = 10 * 60 * 1000;
// 50, not 60. Retries are sends too (0.15.0 shipped forgetting that once
// already), and the escalation-to-reconnect path re-issues everything that was
// in flight, so the budget has to absorb a bad patch without ever reaching the
// number IB actually counts.
inline constexpr int kHistMaxPerWindow = 50;
// The reserve above that ceiling, and what it is reserved FOR.
//
// A budget hold lasts until sends age out of the ten-minute window, which is
// longer than every deadline downstream of it. A single FIFO with no priority
// therefore lets the cheapest possible traffic — the lineup's 30-request ranking
// pass, whose answers are 21 daily bars each — starve the two things that must
// never wait: a live session's warmup fetch, and a chart an operator is looking
// at. An unseeded live strategy is the documented root cause of a day with zero
// trades (a 109-1000 bar lookback against a warmup that restarts at 0), which is
// a far worse outcome than one bulk fetch being late.
//
// 56 keeps four requests of headroom under the 60 IB actually counts, and only
// ReqPriority::Live may reach into the gap. Bulk work stops at 50 exactly as
// before, so the reserve cannot be consumed by the thing it protects against.
inline constexpr int kHistMaxPerWindowLive = 56;

// What a request is FOR — the only thing that can decide whether it may draw on
// the reserve, and the only thing the caller knows that the gate cannot infer.
// Declared on IMarketData::request_candles (net/market_source.h).
enum class ReqPriority {
    Bulk,   // optimizer sweep, lineup ranking pass: nothing is blocked on it
    Live,   // live-session warmup, or a chart a human is waiting on
};

// How long a request may sit in the send queue, held by the gate, before it is
// errored back to its caller instead of eventually going out.
//
// Deliberately LONGER than the tournament's own 75 s fetch budget
// (kTournFetchTimeoutS, panels/sweep.h), so nothing is abandoned while its
// caller is still waiting for it — and finite, because a held request is
// otherwise re-queued forever. A budget spent on fetches everyone has already
// given up on is a budget the next build does not have: on 2026-08-10 the
// tournament's abandoned candidate fetches were exactly what kept going out.
inline constexpr int64_t kHistQueueMaxWaitMs = 90'000;
// Minimum spacing between any two historical sends. 500 ms spreads the
// 30-request opening burst across 15 s — past the ~5 s danger zone the
// measurement above identified, and cheap: a whole build issues ~36 requests, so
// the spacing costs it ~18 s total in the worst case where every one is queued
// back to back. Going to the sustainable 10 s would cost six minutes.
inline constexpr int64_t kHistMinGapMs = 500;
// IB's own documented rule. Deliberately >= 15 s exactly as written, not a
// rounded-up "safe" value, so the test that pins it fails if someone edits it
// toward IB's actual threshold rather than away from it.
inline constexpr int64_t kHistIdenticalGapMs = 15'000;

// Why a send is being held back, so the caller can log something an operator can
// act on rather than "deferred".
enum class SendHold {
    None,        // clear to send
    MinGap,      // too soon after the previous send of anything
    Identical,   // same request within IB's 15s identical-request window
    Budget,      // 50 sends already inside the rolling 10-minute window
};

// The aggregate send gate. Owned by the I/O thread alongside HistRequests and,
// like it, carries no mutex.
//
// `key` is the request's WIRE identity — hist_key(symbol, bar size, duration),
// built above. It must be the same string for two requests IB would consider
// identical and a different one otherwise, which is why it is the resolved pair
// and not the caller's (interval, range); net/bar_cache.h keys on the same
// function, so the two agree by construction.
class HistSendGate {
public:
    // Why (if at all) a send of `key` must wait. Records nothing.
    SendHold hold(const std::string& key, int64_t now_ms,
                  ReqPriority prio = ReqPriority::Bulk) const {
        if (last_send_ms_ != kNever && now_ms - last_send_ms_ < kHistMinGapMs)
            return SendHold::MinGap;
        const auto it = last_by_key_.find(key);
        if (it != last_by_key_.end() && now_ms - it->second < kHistIdenticalGapMs)
            return SendHold::Identical;
        if (in_window(now_ms) >= budget_cap(prio)) return SendHold::Budget;
        return SendHold::None;
    }

    // The ceiling this class of request stops at. See kHistMaxPerWindowLive.
    static int budget_cap(ReqPriority prio) {
        return prio == ReqPriority::Live ? kHistMaxPerWindowLive : kHistMaxPerWindow;
    }

    // Record a send. Call this ONLY when the request actually goes on the wire —
    // including retries, which are sends like any other.
    void record(const std::string& key, int64_t now_ms) {
        last_send_ms_ = now_ms;
        last_by_key_[key] = now_ms;
        sends_.push_back(now_ms);
        prune(now_ms);
    }

    // hold()==None followed by record(). The form callers should use, so the two
    // halves cannot drift apart.
    bool try_send(const std::string& key, int64_t now_ms,
                  ReqPriority prio = ReqPriority::Bulk) {
        if (hold(key, now_ms, prio) != SendHold::None) return false;
        record(key, now_ms);
        return true;
    }

    // Session teardown. The budget is IB's view of THIS client's traffic, and a
    // reconnect gets a new client — but the identical-request window is about
    // the same series being asked for twice in 15 s, which a reconnect does not
    // excuse (drop_connection re-queues in-flight requests, so a teardown is
    // exactly when duplicates appear). Keep the per-key stamps; drop the budget.
    void session_lost() {
        sends_.clear();
        last_send_ms_ = kNever;
    }

    // Diagnostics: sends counted inside the rolling window right now.
    int window_sends(int64_t now_ms) const { return in_window(now_ms); }

private:
    static constexpr int64_t kNever = INT64_MIN / 4;

    int in_window(int64_t now_ms) const {
        int n = 0;
        for (auto it = sends_.rbegin(); it != sends_.rend(); ++it) {
            if (now_ms - *it > kHistWindowMs) break;   // sends_ is time-ordered
            ++n;
        }
        return n;
    }

    void prune(int64_t now_ms) {
        while (!sends_.empty() && now_ms - sends_.front() > kHistWindowMs)
            sends_.pop_front();
        // Per-key stamps are only interesting for kHistIdenticalGapMs, but a
        // long-running process cycles through hundreds of symbols. Sweep them on
        // the same schedule as the window so the map cannot grow without bound.
        if (last_by_key_.size() < 256) return;
        for (auto it = last_by_key_.begin(); it != last_by_key_.end();) {
            if (now_ms - it->second > kHistIdenticalGapMs) it = last_by_key_.erase(it);
            else ++it;
        }
    }

    std::deque<int64_t> sends_;                          // send times, oldest first
    std::unordered_map<std::string, int64_t> last_by_key_;
    int64_t last_send_ms_ = kNever;
};

} // namespace tt::net
