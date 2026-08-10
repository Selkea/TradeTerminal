#pragma once

// The id lifecycle of in-flight reqHistoricalData requests, split out of
// tws_data.cpp so the retry policy can be tested without a gateway (the Io
// struct that owns it is declared only in the .cpp and needs a live
// EClientSocket to exist at all).
//
// WHY THE IDS ARE THE WHOLE PROBLEM. 0.13.x retried a dead history request by
// calling cancelHistoricalData(N) and then re-issuing on the SAME reqId N in
// the same io_loop pass. IB acknowledges a cancel with error 162 ("API
// historical data query cancelled: N") on the NEXT processMsgs — by which time
// N is the FRESHLY RE-ISSUED request, so error() looked N up, found the retry,
// and erased it. Measured over the 2026-08-10 session: 520 "cancelled and
// retrying" lines against 512 "query cancelled" lines, same symbol, same
// second, every time. Three consequences, all observed:
//
//   1. Bars IB *did* return for the retried id were dropped on the floor:
//      historicalData/historicalDataEnd early-return on an unknown reqId. The
//      app could never observe whether its own retry had worked.
//   2. The "died a second time" escalation became unreachable dead code — zero
//      "even after a retry" lines in 8027 log lines despite 520 retries. That
//      escalation is the data-session reconnect which had recovered this exact
//      failure 286 times in the sessions BEFORE the retry feature landed (its
//      last firing is the log line immediately preceding the first-ever retry
//      line). The retry silently replaced a working recovery with none.
//   3. /diag's pending_history and oldest_history_age_ms read 0 straight
//      through a 184-minute outage, because the same over-eager erase emptied
//      the pending set while worst_last_bar_age_ms climbed to 11,059,982 ms.
//
// So the rule this class enforces: a retry is issued under a NEW reqId, and the
// OLD id is remembered just long enough to absorb its own cancel
// acknowledgement — never long enough to leak, and never counted as pending.

#include "market_data.h"   // tt::Candle
#include "net/hist_pacing.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tt::net {

// How long a cancelled id stays around waiting for its error-162 ack. The ack
// arrives on the very next processMsgs (sub-second in every sample), so this is
// pure headroom: it exists only so a NEVER-delivered ack cannot pin an entry in
// the table forever. At the observed ~520 retries/session this bounds the map
// at a handful of entries.
inline constexpr int64_t kCancelAckGraceMs = 60'000;

// One outstanding history fetch.
struct HistPending {
    // The id the CALLER was handed by request_candles. It SURVIVES every retry,
    // because the caller (and the "feed error (req N)" log line) knows nothing
    // about the TWS-side id we swap underneath it.
    uint32_t pub_id = 0;
    std::string symbol, interval;
    std::string dur, bar;        // resolved IB strings, kept so we can re-issue
    std::vector<Candle> candles;
    // Last issue, in steady ms: this is what the timeout classifier measures,
    // so a retry gets a full fresh window rather than dying instantly.
    int64_t sent_ms = 0;
    // First issue, in steady ms: what /diag reports. The outage the operator
    // needs to see is "this symbol has had no bars for 4 minutes", not "the
    // current attempt is 3 seconds old" — see consequence 3 above.
    int64_t first_sent_ms = 0;
    bool retried = false;        // already cancelled + re-issued once
};

class HistRequests {
public:
    // ---- the live (genuinely outstanding) set -------------------------------
    void add(int req_id, HistPending p) { live_[req_id] = std::move(p); }

    HistPending* find(int req_id) {
        const auto it = live_.find(req_id);
        return it == live_.end() ? nullptr : &it->second;
    }
    const HistPending* find(int req_id) const {
        const auto it = live_.find(req_id);
        return it == live_.end() ? nullptr : &it->second;
    }

    void erase(int req_id) { live_.erase(req_id); }

    // Session teardown: nothing outstanding survives it, and neither does an
    // ack we will now never be sent (the socket that owed it to us is gone).
    void clear() {
        live_.clear();
        cancelled_.clear();
    }

    const std::unordered_map<int, HistPending>& live() const { return live_; }

    // ---- the retry ----------------------------------------------------------
    // Ids whose request has blown the timeout and has NOT been retried yet.
    // Returned as a list rather than walked in place because begin_retry()
    // rehashes the map underneath the caller.
    std::vector<int> dead_ids(int64_t now_ms) const {
        std::vector<int> out;
        for (const auto& [id, p] : live_)
            if (history_action(now_ms - p.sent_ms, p.retried) == HistAction::Retry)
                out.push_back(id);
        return out;
    }

    // Move the request from `old_id` to `new_id` and park `old_id` in the
    // awaiting-ack set. Returns the relocated request (nullptr if old_id was
    // not live). The caller cancels old_id and re-issues on new_id; the ack for
    // old_id then lands on take_cancel_ack() instead of destroying the retry.
    HistPending* begin_retry(int old_id, int new_id, int64_t now_ms) {
        const auto it = live_.find(old_id);
        if (it == live_.end()) return nullptr;
        HistPending p = std::move(it->second);
        live_.erase(it);
        p.retried = true;
        p.sent_ms = now_ms;   // fresh window; first_sent_ms deliberately kept
        p.candles.clear();    // anything IB sent for old_id is not our answer
        cancelled_[old_id] = now_ms;
        auto [ins, ok] = live_.emplace(new_id, std::move(p));
        (void)ok;
        return &ins->second;
    }

    // Is this id a request we cancelled and are still expecting IB's error-162
    // acknowledgement for? Consumes the entry: exactly one ack is owed.
    bool take_cancel_ack(int id) { return cancelled_.erase(id) > 0; }

    // Drop acks that never came. Without this a gateway that swallows the
    // acknowledgement would grow the set by one entry per retry, forever.
    void prune_cancel_acks(int64_t now_ms) {
        for (auto it = cancelled_.begin(); it != cancelled_.end();) {
            if (now_ms - it->second > kCancelAckGraceMs) it = cancelled_.erase(it);
            else ++it;
        }
    }

    // ---- escalation + diagnostics ------------------------------------------
    // A request that has blown the timeout for the SECOND time: the retry did
    // not help, so the session itself is suspect and the caller should do the
    // (expensive) data-session reconnect. Unreachable before 0.15.0 — see the
    // header note.
    bool has_twice_dead(int64_t now_ms) const {
        for (const auto& [id, p] : live_)
            if (history_action(now_ms - p.sent_ms, p.retried) == HistAction::Escalate)
                return true;
        return false;
    }

    // /diag: genuinely outstanding requests. A cancelled-awaiting-ack id is NOT
    // one — its replacement is already in the live set and counting it too
    // would double-report the same fetch.
    int pending() const { return static_cast<int>(live_.size()); }

    // Age of the longest-running outstanding fetch, measured from its FIRST
    // issue so retries do not reset the outage /diag is trying to show.
    int oldest_age_ms(int64_t now_ms) const {
        int64_t oldest = 0;
        for (const auto& [id, p] : live_) {
            const int64_t age = now_ms - p.first_sent_ms;
            if (age > oldest) oldest = age;
        }
        return static_cast<int>(oldest);
    }

    // Test/diagnostic accessor: how many cancelled ids are still owed an ack.
    size_t awaiting_ack() const { return cancelled_.size(); }

private:
    std::unordered_map<int, HistPending> live_;
    // dead reqId -> when we cancelled it (steady ms).
    std::unordered_map<int, int64_t> cancelled_;
};

} // namespace tt::net
