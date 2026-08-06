// Pacing rules for IB historical-data requests. These encode a measurement, not
// a preference — see net/hist_pacing.h for the 178-request sample they came
// from. The previous behaviour tore the whole data session down (~75 times a
// day, destroying and re-subscribing every quote stream) for what is actually
// one request IB declined to answer while its siblings on the same socket
// succeeded.
#include "doctest.h"

#include "net/feed_order.h"

#include <string>
#include <vector>

// ---- feed fidelity ordering ------------------------------------------------
TEST_CASE("feed order: close-only strategies take the sampled slots") {
    using tt::ui::feed_fidelity_rank;
    // Tick-driven needs the tape most.
    CHECK(feed_fidelity_rank("scalper_burst.cpp") <
          feed_fidelity_rank("orb_breakout.cpp"));
    // High/low readers beat close-only readers.
    CHECK(feed_fidelity_rank("orb_breakout.cpp") <
          feed_fidelity_rank("bollinger_reversion.cpp"));
    CHECK(feed_fidelity_rank("donchian_trend.cpp") <
          feed_fidelity_rank("rsi2_pullback.cpp"));
    // An unclassified strategy must NOT be demoted into the sampled slot.
    CHECK(feed_fidelity_rank("something_new.cpp") <
          feed_fidelity_rank("bollinger_reversion.cpp"));
    // The built-in ("") reads closes only.
    CHECK(feed_fidelity_rank("") == feed_fidelity_rank("sma_crossover.cpp"));
}

TEST_CASE("feed order: sorting is stable and puts close-only last") {
    struct S {
        std::string sym, key;
    };
    // The live 2026-08-05 lineup, in the order it actually ran.
    std::vector<S> v{{"SNXX", "orb_breakout.cpp"},  {"MUU", "orb_breakout.cpp"},
                     {"SOXS", "orb_breakout.cpp"},  {"SNDQ", "rsi2_pullback.cpp"},
                     {"AMIX", "orb_breakout.cpp"},  {"SPCH", "bollinger_reversion.cpp"}};
    tt::ui::order_by_feed_fidelity(v, [](const S& s) { return s.key; });
    // Every high/low reader ahead of every close-only one...
    CHECK(v[0].sym == "SNXX");
    CHECK(v[1].sym == "MUU");
    CHECK(v[2].sym == "SOXS");
    CHECK(v[3].sym == "AMIX");   // was 5th; promoted past the close-only pair
    // ...and the close-only pair keeps its original relative order.
    CHECK(v[4].sym == "SNDQ");
    CHECK(v[5].sym == "SPCH");
}

#include "net/hist_pacing.h"

using namespace tt::net;

TEST_CASE("a small preceding delivery does not block the next request") {
    // 16/16 alive in the sample, even at a sub-second gap.
    CHECK_FALSE(history_send_blocked(2'163, 0));
    CHECK_FALSE(history_send_blocked(2'163, 4'000));
    CHECK_FALSE(history_send_blocked(0, 0));
}

TEST_CASE("a large preceding delivery blocks until the quiet window passes") {
    // 76/93 dead in the sample at a <=5s gap after a >=3000-bar batch.
    CHECK(history_send_blocked(7'700, 0));
    CHECK(history_send_blocked(7'700, 4'000));
    CHECK(history_send_blocked(3'000, 9'999));      // exactly at the bar threshold
    CHECK_FALSE(history_send_blocked(7'700, 10'000));   // window elapsed
    CHECK_FALSE(history_send_blocked(7'700, 60'000));
}

TEST_CASE("it is the preceding delivery that matters, not the gap alone") {
    // The pair that rules out send-spacing as the explanation: KORU survived a
    // 4s gap after a 2,163-bar batch; SNXX died at a 9s gap after 7,700 bars.
    CHECK_FALSE(history_send_blocked(2'163, 4'000));
    CHECK(history_send_blocked(7'700, 9'000));
}

TEST_CASE("a request inside its normal turnaround is left alone") {
    // Max observed successful latency was 17s, so nothing under the timeout is
    // treated as dead however slow it looks.
    CHECK(history_action(0, false) == HistAction::Wait);
    CHECK(history_action(17'000, false) == HistAction::Wait);
    CHECK(history_action(kHistTimeoutMs, false) == HistAction::Wait);
    CHECK(history_action(17'000, true) == HistAction::Wait);
}

TEST_CASE("a first death is retried in place, not escalated") {
    CHECK(history_action(kHistTimeoutMs + 1, false) == HistAction::Retry);
    CHECK(history_action(60'000, false) == HistAction::Retry);
}

TEST_CASE("a second death escalates to a session teardown") {
    // Only after cancel-and-reissue has already failed is the session itself
    // suspect — that is what the (expensive) drop_connection is reserved for.
    CHECK(history_action(kHistTimeoutMs + 1, true) == HistAction::Escalate);
    CHECK(history_action(60'000, true) == HistAction::Escalate);
}

TEST_CASE("the quiet window stays clear of the death timeout") {
    // A blocked send must cost an extra loop pass, never a failed fetch: if the
    // quiet window ever reached the timeout, holding a request back would itself
    // mark it dead.
    CHECK(kBigBatchQuietMs < kHistTimeoutMs);
}
