// FeedBackoff: the reconnect pacing shared by finnhub, polygon and ibkr.
//
// All three had the same defect written out three times — a dropped session
// retried on a hardcoded 1000 ms while only a failed HANDSHAKE backed off, and
// every successful handshake reset the backoff. A link that accepted the
// connection and then died immediately logged "stream lost, reconnecting" once
// a second forever, which classify_alert rates Warning: 60 pages a minute.
//
// The rule under test is one sentence: a handshake is not proof of health, a
// session earns the reset by LASTING.
#include "doctest.h"

#include "engine/feed_reconnect.h"

using tt::FeedBackoff;
using tt::kFeedHealthySessionMs;
using tt::kFeedMaxBackoffSec;

TEST_CASE("FeedBackoff: a failing handshake backs off the way it always did") {
    // Unchanged behaviour, pinned so the refactor cannot have altered it.
    FeedBackoff b;
    CHECK(b.on_handshake_failed() == 1);
    CHECK(b.on_handshake_failed() == 2);
    CHECK(b.on_handshake_failed() == 4);
    CHECK(b.on_handshake_failed() == 8);
}

TEST_CASE("FeedBackoff: the backoff is capped") {
    FeedBackoff b;
    for (int i = 0; i < 20; ++i) b.on_handshake_failed();
    CHECK(b.current() == kFeedMaxBackoffSec);
    CHECK(b.on_handshake_failed() == kFeedMaxBackoffSec);
}

TEST_CASE("FeedBackoff: a session that dies instantly does NOT reset the backoff") {
    // THE BUG. Connect, die, connect, die — the old loop reset to 1 on every
    // handshake and then waited a hardcoded second, so this ran at 1 Hz with a
    // Warning-tier log line on every pass.
    FeedBackoff b;
    int64_t t = 0;
    int waits[5];
    for (int i = 0; i < 5; ++i) {
        b.on_connected(t);
        t += 20;                        // the session lasts 20 ms
        waits[i] = b.on_session_lost(t);
    }
    CHECK(waits[0] == 1);
    CHECK(waits[1] == 2);
    CHECK(waits[2] == 4);
    CHECK(waits[3] == 8);
    CHECK(waits[4] == 16);
}

TEST_CASE("FeedBackoff: a flapping link settles at one retry per 30s, not one per second") {
    // The property that matters operationally: over ten minutes of continuous
    // flapping the feed reconnects a couple of dozen times, not six hundred.
    FeedBackoff b;
    int64_t t = 0;
    int attempts = 0;
    while (t < 600'000) {
        b.on_connected(t);
        t += 20;                        // dies immediately, every time
        t += b.on_session_lost(t) * 1000;
        ++attempts;
    }
    CHECK(attempts < 30);               // the old loop managed ~600
    CHECK(b.current() == kFeedMaxBackoffSec);
}

TEST_CASE("FeedBackoff: a session that HELD earns an immediate retry") {
    // The ordinary case must not be punished: a feed that streamed all morning
    // and dropped once reconnects in a second, exactly as before.
    FeedBackoff b;
    int64_t t = 0;
    b.on_connected(t);
    t += kFeedHealthySessionMs;         // it held
    CHECK(b.on_session_lost(t) == 1);
}

TEST_CASE("FeedBackoff: a long session clears a backoff built up by earlier failures") {
    // Connect attempts fail for a while, then one succeeds and holds. The next
    // drop is an ordinary reconnect and must not inherit the old penalty.
    FeedBackoff b;
    for (int i = 0; i < 6; ++i) b.on_handshake_failed();
    REQUIRE(b.current() == kFeedMaxBackoffSec);
    int64_t t = 1'000'000;
    b.on_connected(t);
    t += kFeedHealthySessionMs * 10;    // streamed for five minutes
    CHECK(b.on_session_lost(t) == 1);
}

TEST_CASE("FeedBackoff: the healthy threshold is a floor, not an approximation") {
    // One millisecond short must not count, or the rule is decorative.
    {
        FeedBackoff b;
        b.on_connected(0);
        CHECK(b.on_session_lost(kFeedHealthySessionMs - 1) == 1);   // first wait is 1 either way
        CHECK(b.current() == 2);                                    // ...but it CLIMBED
    }
    {
        FeedBackoff b;
        b.on_connected(0);
        CHECK(b.on_session_lost(kFeedHealthySessionMs) == 1);
        CHECK(b.current() == 2);
    }
    // The difference shows on the SECOND drop: a short session keeps climbing,
    // a healthy one is pulled back to 1.
    {
        FeedBackoff shortlived, healthy;
        shortlived.on_connected(0);
        shortlived.on_session_lost(10);
        shortlived.on_connected(10);
        CHECK(shortlived.on_session_lost(20) == 2);
        healthy.on_connected(0);
        healthy.on_session_lost(kFeedHealthySessionMs);
        healthy.on_connected(kFeedHealthySessionMs);
        CHECK(healthy.on_session_lost(kFeedHealthySessionMs * 2) == 1);
    }
}

TEST_CASE("FeedBackoff: a drop with no session before it is still paced") {
    // Defensive: on_session_lost without a preceding on_connected must not
    // reset anything on the strength of an uninitialised timestamp.
    FeedBackoff b;
    for (int i = 0; i < 4; ++i) b.on_handshake_failed();
    REQUIRE(b.current() == 16);
    CHECK(b.on_session_lost(999'999'999) == 16);   // no reset from a session that never was
    CHECK(b.current() == kFeedMaxBackoffSec);      // 32, clamped
}
