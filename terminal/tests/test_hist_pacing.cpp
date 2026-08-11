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
    CHECK(history_action(kHistEscalateMs, false) == HistAction::Retry);
}

TEST_CASE("a second death escalates to a session teardown") {
    // Only after cancel-and-reissue has already failed is the session itself
    // suspect — that is what the (expensive) drop_connection is reserved for.
    CHECK(history_action(kHistTimeoutMs + 1, true) == HistAction::Escalate);
    CHECK(history_action(60'000, true) == HistAction::Escalate);
}

TEST_CASE("silence long enough to have died twice escalates even with no retry") {
    // The retry is not the point; the RECOVERY is. `already_retried` is set only
    // when HistRequests::begin_retry runs, and retry_dead_history has to get past
    // HistSendGate first. A SendHold::Budget hold lasts until sends age out of a
    // ten-minute window, so keying the escalation purely off `already_retried`
    // meant a spent budget suppressed the data-session reconnect as well as the
    // retry — and a degraded lineup build is precisely what spends the budget.
    // Recovery went from ~40s to ~10 minutes in the one scenario it exists for.
    CHECK(history_action(kHistEscalateMs + 1, false) == HistAction::Escalate);
    CHECK(history_action(600'000, false) == HistAction::Escalate);
    // The escalation age is the natural timeline it replaces: one timeout to die,
    // one more to die again after an immediate retry. Anything shorter would
    // escalate a request whose retry is simply still in flight.
    CHECK(kHistEscalateMs == 2 * kHistTimeoutMs);
    // ...and it must be comfortably past the longest legitimate deferral, or
    // waiting out the big-batch quiet window would itself tear the session down.
    CHECK(kHistEscalateMs > kHistTimeoutMs + kBigBatchQuietMs);
}

TEST_CASE("the quiet window stays clear of the death timeout") {
    // A blocked send must cost an extra loop pass, never a failed fetch: if the
    // quiet window ever reached the timeout, holding a request back would itself
    // mark it dead.
    CHECK(kBigBatchQuietMs < kHistTimeoutMs);
}

// ---- aggregate send rate ---------------------------------------------------
//
// The 2026-08-10 lineup build opened with 30 'candles: SYM 1d x21' deliveries
// stamped in the SAME SECOND and put ~60 historical requests into a window IB
// caps at 60 per 10 minutes. The data session went silent at 09:40:41 and stayed
// silent for 20 minutes; a fresh client at 10:00:43 was served instantly.

TEST_CASE("the first request of a session goes straight out") {
    HistSendGate g;
    CHECK(g.hold("SPY\x1f""5m\x1f""6mo", 0) == SendHold::None);
    CHECK(g.try_send("SPY\x1f""5m\x1f""6mo", 0));
    CHECK(g.window_sends(0) == 1);
}

TEST_CASE("the 30-request opening burst is spread, not sent in one second") {
    HistSendGate g;
    // The literal shape of the burst: 30 different symbols queued together.
    int64_t now = 0;
    int sent_in_first_second = 0;
    for (int i = 0; i < 30; ++i)
        if (g.try_send("SYM" + std::to_string(i), now)) ++sent_in_first_second;
    CHECK(sent_in_first_second == 1);
    // Walking the clock forward at the minimum gap gets all 30 out, in order,
    // across 15 seconds rather than 30 in one.
    int sent = 1;
    for (int i = 1; i < 30; ++i) {
        now += kHistMinGapMs;
        if (g.try_send("SYM" + std::to_string(i), now)) ++sent;
    }
    CHECK(sent == 30);
    CHECK(now == 29 * kHistMinGapMs);
    CHECK(now <= 15'000);
}

TEST_CASE("the minimum gap is enforced between any two requests") {
    HistSendGate g;
    REQUIRE(g.try_send("A", 1'000));
    CHECK(g.hold("B", 1'000) == SendHold::MinGap);
    CHECK(g.hold("B", 1'000 + kHistMinGapMs - 1) == SendHold::MinGap);
    CHECK_FALSE(g.try_send("B", 1'000 + kHistMinGapMs - 1));
    CHECK(g.hold("B", 1'000 + kHistMinGapMs) == SendHold::None);
    CHECK(g.try_send("B", 1'000 + kHistMinGapMs));
}

TEST_CASE("a held request records nothing, so it does not shift the next gap") {
    HistSendGate g;
    REQUIRE(g.try_send("A", 0));
    CHECK_FALSE(g.try_send("B", 100));    // rejected...
    CHECK_FALSE(g.try_send("B", 200));    // ...and did not restart the clock
    CHECK(g.try_send("B", kHistMinGapMs));
    CHECK(g.window_sends(kHistMinGapMs) == 2);
}

TEST_CASE("an IDENTICAL request inside 15s is refused, per IB's own rule") {
    // SSPC 5m/6mo, twice, 12 seconds apart — the exact violation in the build.
    HistSendGate g;
    const std::string sspc = "SSPC\x1f""5m\x1f""6mo";
    REQUIRE(g.try_send(sspc, 0));
    // Well past the minimum gap, so this is the identical rule and nothing else.
    CHECK(g.hold(sspc, 12'000) == SendHold::Identical);
    CHECK_FALSE(g.try_send(sspc, 12'000));
    CHECK(g.hold(sspc, kHistIdenticalGapMs - 1) == SendHold::Identical);
    CHECK(g.hold(sspc, kHistIdenticalGapMs) == SendHold::None);
    CHECK(g.try_send(sspc, kHistIdenticalGapMs));
}

TEST_CASE("a DIFFERENT request is not blocked by the identical rule") {
    HistSendGate g;
    REQUIRE(g.try_send("SSPC\x1f""5m\x1f""6mo", 0));
    // Same symbol, different series: a warmup fetch is not the 6-month fetch.
    CHECK(g.hold("SSPC\x1f""5m\x1f""1d", 12'000) == SendHold::None);
    CHECK(g.try_send("SSPC\x1f""5m\x1f""1d", 12'000));
}

TEST_CASE("the identical window is at least IB's documented 15 seconds") {
    // Pinned as a value, not just as behaviour: this one is IBKR's number, and
    // editing it downward is a pacing violation however reasonable it looks.
    CHECK(kHistIdenticalGapMs >= 15'000);
}

TEST_CASE("the rolling 10-minute budget stops at IB's ceiling, with headroom") {
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindow; ++i) {
        REQUIRE(g.try_send("SYM" + std::to_string(i), now));
        now += kHistMinGapMs;
    }
    CHECK(g.window_sends(now) == kHistMaxPerWindow);
    CHECK(g.hold("ONE_MORE", now) == SendHold::Budget);
    CHECK_FALSE(g.try_send("ONE_MORE", now));
    // Retries and a reconnect's re-queue are sends too, so the ceiling must sit
    // BELOW the number IB actually counts.
    CHECK(kHistMaxPerWindow < 60);
}

TEST_CASE("the budget frees up as sends age out of the window") {
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindow; ++i) {
        REQUIRE(g.try_send("SYM" + std::to_string(i), now));
        now += kHistMinGapMs;
    }
    REQUIRE(g.hold("LATER", now) == SendHold::Budget);
    // The first send ages out one window after it was made, not after the last.
    CHECK(g.hold("LATER", kHistWindowMs) == SendHold::Budget);
    CHECK(g.hold("LATER", kHistWindowMs + 1) == SendHold::None);
    CHECK(g.try_send("LATER", kHistWindowMs + 1));
}

TEST_CASE("the minimum gap is checked before the budget") {
    // Ordering matters for the log line the operator reads: "already used 50 of
    // IB's 60" is a real diagnosis and "500 ms early" is noise, so the cheap
    // ordinary case must not be reported as the alarming one.
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindow; ++i) {
        REQUIRE(g.try_send("SYM" + std::to_string(i), now));
        now += kHistMinGapMs;
    }
    CHECK(g.hold("X", now - kHistMinGapMs + 1) == SendHold::MinGap);
}

TEST_CASE("a session teardown clears the budget but keeps the identical window") {
    // The budget is IB's accounting for a client that no longer exists, so a
    // reconnect starts clean. The identical-request window is not about the
    // client at all — and drop_connection re-queues everything that was in
    // flight, so a teardown is exactly when duplicates appear.
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindow; ++i) {
        REQUIRE(g.try_send("SYM" + std::to_string(i), now));
        now += kHistMinGapMs;
    }
    REQUIRE(g.hold("FRESH", now) == SendHold::Budget);
    g.session_lost();
    CHECK(g.window_sends(now) == 0);
    CHECK(g.hold("FRESH", now) == SendHold::None);      // budget released
    CHECK(g.hold("SYM0", now) == SendHold::None);       // sent long ago, aged out
    // SYM49 went out on the last tick before the teardown; still inside 15s.
    CHECK(g.hold("SYM" + std::to_string(kHistMaxPerWindow - 1), now) ==
          SendHold::Identical);
}

TEST_CASE("the minimum gap cannot exceed the identical window") {
    // If it did, the identical rule would be unreachable — every request would
    // already have been held by the cheaper check, and the log would never name
    // the pacing rule that was actually being broken.
    CHECK(kHistMinGapMs < kHistIdenticalGapMs);
    // And the spacing must stay far under a fetch's own death timeout, or
    // pacing a request would be what kills it.
    CHECK(kHistMinGapMs < kHistTimeoutMs);
}

TEST_CASE("the whole build's request load fits inside one window") {
    // A lineup build is 30 daily-bar requests for the ranking pass plus one 5m
    // series per pick (6), once the cache removes the duplicates. If that did not
    // fit in the budget, the build would stall on its own pacing.
    CHECK(30 + 6 < kHistMaxPerWindow);
}

// ---- what makes two requests "the same request" ----------------------------

TEST_CASE("the request identity is the RESOLVED duration, not the caller's range") {
    // max_dur_idx caps 1h bars at 2 years, so a chart asking for "5y" and one
    // asking for "max" send the SAME reqHistoricalData: dur="2 Y", bar="1 hour".
    // Keyed on the caller's label they looked like different requests, so the
    // gate cleared the second and two byte-identical calls went out kHistMinGapMs
    // apart — IB's own definition of a pacing violation, committed by the gate
    // that exists to prevent it.
    const HistWire five_y = hist_wire("1h", "5y");
    const HistWire maximum = hist_wire("1h", "max");
    REQUIRE(five_y.ok());
    REQUIRE(maximum.ok());
    CHECK(std::string(five_y.dur) == "2 Y");
    CHECK(std::string(maximum.dur) == "2 Y");
    CHECK(five_y.clamped);
    CHECK(maximum.clamped);
    CHECK(hist_key("SPY", five_y) == hist_key("SPY", maximum));

    HistSendGate g;
    REQUIRE(g.try_send(hist_key("SPY", five_y), 0));
    CHECK(g.hold(hist_key("SPY", maximum), kHistMinGapMs) == SendHold::Identical);
}

TEST_CASE("requests that resolve differently stay different") {
    // The mirror-image danger, and the worse one: a warmup fetch and a six-month
    // optimization of the same symbol at the same interval are NOT the same
    // series (1567 bars against ~2966-9517 on 2026-08-10), and answering one with
    // the other is how the optimizer crowns a champion on the wrong data.
    const HistWire warmup = hist_wire("5m", "1mo");
    const HistWire opt = hist_wire("5m", "6mo");
    REQUIRE(warmup.ok());
    REQUIRE(opt.ok());
    CHECK(std::string(warmup.dur) == "1 M");
    CHECK(std::string(opt.dur) == "6 M");
    CHECK(hist_key("SOXL", warmup) != hist_key("SOXL", opt));
    // Different bar size, same span: also different.
    CHECK(hist_key("SOXL", hist_wire("1d", "6mo")) != hist_key("SOXL", opt));
    // Different symbol, everything else equal.
    CHECK(hist_key("KORU", opt) != hist_key("SOXL", opt));
    // A range under the cap is not clamped and keeps its own duration.
    CHECK_FALSE(opt.clamped);
}

TEST_CASE("an unknown interval or range resolves to nothing") {
    CHECK_FALSE(hist_wire("7m", "6mo").ok());
    CHECK_FALSE(hist_wire("5m", "3mo").ok());
    CHECK(hist_wire("5m", "6mo").ok());
}

// ---- the reserve -----------------------------------------------------------

TEST_CASE("a live warmup fetch is not starved by a bulk pass that spent the budget") {
    // The failure this exists for: the lineup's 30-request ranking pass fills the
    // window, then a live session starts cold and its warmup fetches are held
    // until sends age out — up to ten minutes. A strategy with a several-hundred
    // -bar lookback and no warmup cannot trade at all that session, which is a
    // far worse outcome than one 21-bar ranking series arriving late.
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindow; ++i) {
        REQUIRE(g.try_send("BULK" + std::to_string(i), now, ReqPriority::Bulk));
        now += kHistMinGapMs;
    }
    CHECK(g.hold("SOXL", now, ReqPriority::Bulk) == SendHold::Budget);
    CHECK(g.hold("SOXL", now, ReqPriority::Live) == SendHold::None);
    CHECK(g.try_send("SOXL", now, ReqPriority::Live));
}

TEST_CASE("the reserve is finite, and bulk work can never reach into it") {
    HistSendGate g;
    int64_t now = 0;
    for (int i = 0; i < kHistMaxPerWindowLive; ++i) {
        REQUIRE(g.try_send("SYM" + std::to_string(i), now, ReqPriority::Live));
        now += kHistMinGapMs;
    }
    // Even a live request stops at the reserve ceiling.
    CHECK(g.hold("ONE_MORE", now, ReqPriority::Live) == SendHold::Budget);
    // Bulk stopped 6 sends ago and stays stopped.
    CHECK(g.hold("BULK", now, ReqPriority::Bulk) == SendHold::Budget);
    // The ceilings bracket correctly: bulk below live, live below IB's 60.
    CHECK(kHistMaxPerWindow < kHistMaxPerWindowLive);
    CHECK(kHistMaxPerWindowLive < 60);
    // And Bulk is the default, so a caller that says nothing cannot take reserve.
    CHECK(HistSendGate::budget_cap(ReqPriority::Bulk) == kHistMaxPerWindow);
    CHECK(HistSendGate::budget_cap(ReqPriority::Live) == kHistMaxPerWindowLive);
}

TEST_CASE("a held request is abandoned before it outlives everyone waiting on it") {
    // A gate hold puts the request back on the queue every pass, with no bound.
    // The budget it eventually spends would then go on an answer whose caller
    // gave up long ago — the budget the NEXT symbol's tournament needed. The cut
    // -off has to sit ABOVE the tournament's own 75s fetch budget so nothing is
    // abandoned while its caller is still waiting, and below the ten-minute
    // window so a spent budget cannot spend itself twice.
    CHECK(kHistQueueMaxWaitMs > 75'000);
    CHECK(kHistQueueMaxWaitMs < kHistWindowMs);
}
