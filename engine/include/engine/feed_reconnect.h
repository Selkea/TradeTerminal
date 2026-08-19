#pragma once
// Reconnect pacing shared by every streaming feed (finnhub, polygon, ibkr).
//
// All three had the same defect, written out three times. Their loops backed
// off on a failed HANDSHAKE — 1s, 2s, 4s, up to 30s — but an established
// session that dropped went back to a hardcoded 1000 ms, and every successful
// handshake reset the backoff to 1. So a link that accepted the connection and
// then died immediately (a gateway up but not serving, a websocket the far end
// closes on every subscribe, a network path that blackholes after the upgrade)
// produced this, forever, once per second:
//
//     stream lost, reconnecting
//
// which classify_alert rates Warning. That is 60 pages a minute from a feed
// doing nothing but failing — the same shape as the 73-in-90-seconds burst of
// 2026-08-17, from a different source, and 0.29.1's fix did not cover it: these
// adapter drains are live by definition, so the from_live guard never applies.
//
// The fix is one idea: A HANDSHAKE IS NOT PROOF OF HEALTH. A session earns the
// backoff reset by LASTING, not by starting. Anything shorter than
// kFeedHealthySessionMs leaves the backoff climbing, so a flapping link settles
// at one retry (and one log line) every 30 s instead of one every second.
//
// 0.30.0's coalescing would fold these pages anyway. This still matters: that
// layer bounds what the operator is TOLD, and this bounds what the process
// DOES. A feed reconnecting 60 times a minute is burning a socket, a TLS
// handshake and a subscribe round-trip each time, whether or not anyone is
// paged about it.

#include <algorithm>
#include <cstdint>

namespace tt {

// A session must survive this long for the link to count as healthy.
inline constexpr int64_t kFeedHealthySessionMs = 30000;
inline constexpr int kFeedMaxBackoffSec = 30;

class FeedBackoff {
public:
    // A handshake succeeded. Note this does NOT reset the backoff — only
    // on_session_lost does, and only for a session that lasted.
    void on_connected(int64_t now_ms) {
        session_start_ms_ = now_ms;
        live_ = true;
    }

    // The handshake itself failed. Returns seconds to wait before retrying.
    int on_handshake_failed() { return next(); }

    // An established session dropped. Returns seconds to wait before retrying.
    int on_session_lost(int64_t now_ms) {
        if (live_ && now_ms - session_start_ms_ >= kFeedHealthySessionMs)
            backoff_s_ = 1;   // it held: this is an ordinary reconnect
        live_ = false;
        return next();
    }

    int current() const { return backoff_s_; }

private:
    int next() {
        const int s = backoff_s_;
        backoff_s_ = std::min(backoff_s_ * 2, kFeedMaxBackoffSec);
        return s;
    }

    int backoff_s_ = 1;
    int64_t session_start_ms_ = 0;
    bool live_ = false;
};

} // namespace tt
