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
//   a reqHistoricalData issued within ~5s of a historicalDataEnd that delivered
//   >= ~3000 bars is never answered.
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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace tt::net {

// A delivery at or above this many bars poisons the next request for a while.
inline constexpr size_t kBigBatchBars = 3'000;
// Quiet window after such a delivery. The danger zone measured out at ~5s; 10s
// is comfortably clear of it and still well under the 20s timeout, so a blocked
// request costs at most one extra io_loop pass rather than a failed fetch.
inline constexpr int64_t kBigBatchQuietMs = 10'000;
// Unanswered for this long = dead. See the note above: this is a classifier,
// not a patience setting.
inline constexpr int64_t kHistTimeoutMs = 20'000;

// Should we hold a new history request back right now?
inline bool history_send_blocked(size_t last_batch_bars, int64_t ms_since_last_end) {
    return last_batch_bars >= kBigBatchBars && ms_since_last_end < kBigBatchQuietMs;
}

// What to do with an in-flight request of this age.
enum class HistAction {
    Wait,       // still within its normal turnaround
    Retry,      // dead, but never retried: cancel + re-issue on the same session
    Escalate,   // dead again after a retry: the session itself is suspect
};

inline HistAction history_action(int64_t age_ms, bool already_retried) {
    if (age_ms <= kHistTimeoutMs) return HistAction::Wait;
    return already_retried ? HistAction::Escalate : HistAction::Retry;
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
// `key` is the caller's request identity — symbol + interval + range. It must be
// the same string for two requests that IB would consider identical and a
// different one otherwise; TwsData builds it from exactly the triple that
// net/bar_cache.h keys on, so the two agree by construction.
class HistSendGate {
public:
    // Why (if at all) a send of `key` must wait. Records nothing.
    SendHold hold(const std::string& key, int64_t now_ms) const {
        if (last_send_ms_ != kNever && now_ms - last_send_ms_ < kHistMinGapMs)
            return SendHold::MinGap;
        const auto it = last_by_key_.find(key);
        if (it != last_by_key_.end() && now_ms - it->second < kHistIdenticalGapMs)
            return SendHold::Identical;
        if (in_window(now_ms) >= kHistMaxPerWindow) return SendHold::Budget;
        return SendHold::None;
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
    bool try_send(const std::string& key, int64_t now_ms) {
        if (hold(key, now_ms) != SendHold::None) return false;
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
