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

} // namespace tt::net
