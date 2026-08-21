// TradeJournal: per-session/day PnL is (final_equity - baseline), where the
// baseline is re-anchored to the real account equity after broker
// reconciliation (set_baseline). These pin two bugs that shipped from this file:
//   - 0.2.4: days() referenced a SELECT-list alias inside a correlated subquery,
//     failed to prepare, and returned NO rows — the Day table was always empty.
//   - 0.2.5: begin_session captured a pre-reconciliation placeholder as the PnL
//     baseline, so a restarted TWS session booked the whole ~$1M account as PnL.
#include "doctest.h"

#include "journal.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using tt::ui::TradeJournal;

namespace {
// A private on-disk db per case (WAL needs a real file, not :memory:). Cleared
// up front so a crashed prior run can't leak state in.
std::string fresh_db(const char* tag) {
    const std::string path = std::string("test_journal_") + tag + ".db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    return path;
}
}  // namespace

TEST_CASE("session PnL is final_equity minus the re-anchored baseline") {
    const std::string db = fresh_db("baseline");
    {
        TradeJournal j;
        REQUIRE(j.open(db));
        // Placeholder baseline at start (pre-reconciliation): ~100k notional.
        const int64_t sid = j.begin_session("AAPL,MSFT", "ibkr", 100'000.0);
        REQUIRE(sid != 0);
        // Reconciliation loads the real account -> re-anchor to ~1M.
        j.set_baseline(sid, 1'000'000.0);
        j.add_fill(sid, 1, "AAPL", true, 10, 190.0, 1.0, 111);
        j.add_fill(sid, 2, "AAPL", false, 10, 195.0, 1.0, 112);
        j.end_session(sid, 1'000'500.0, /*halted=*/false);

        const auto rows = j.sessions();
        REQUIRE(rows.size() == 1);
        // 500, not 900500 — measured off the re-anchored baseline.
        CHECK(rows[0].pnl == doctest::Approx(500.0));
        CHECK(rows[0].fills == 2);
        CHECK_FALSE(rows[0].open);
        CHECK_FALSE(rows[0].halted);
    }
    for (const char* s : {"", "-wal", "-shm"}) std::remove((db + s).c_str());
}

TEST_CASE("days() returns a per-day row and sums session PnL (0.2.4 regression)") {
    const std::string db = fresh_db("days");
    {
        TradeJournal j;
        REQUIRE(j.open(db));
        const int64_t a = j.begin_session("AAPL", "ibkr", 1'000'000.0);
        j.end_session(a, 1'000'300.0, false);
        const int64_t b = j.begin_session("MSFT", "ibkr", 1'000'000.0);
        j.end_session(b, 999'900.0, false);

        const auto days = j.days();
        // The prepare-fail bug made this empty forever; require a row exists.
        REQUIRE(days.size() == 1);        // both sessions land on the same local day
        CHECK(days[0].sessions == 2);
        CHECK(days[0].pnl == doctest::Approx(200.0));   // +300 - 100
    }
    for (const char* s : {"", "-wal", "-shm"}) std::remove((db + s).c_str());
}

TEST_CASE("an open session (no end_session) reports open and zero PnL") {
    const std::string db = fresh_db("open");
    {
        TradeJournal j;
        REQUIRE(j.open(db));
        const int64_t sid = j.begin_session("TSLA", "sim", 100'000.0);
        j.add_fill(sid, 1, "TSLA", true, 5, 250.0, 1.0, 1);

        const auto rows = j.sessions();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].open);                    // ended_utc IS NULL
        CHECK(rows[0].pnl == doctest::Approx(0.0));   // final_equity NULL -> COALESCE 0
        CHECK(rows[0].fills == 1);

        const auto fills = j.fills(sid);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].symbol == "TSLA");
        CHECK(fills[0].side == "buy");
        CHECK(fills[0].qty == doctest::Approx(5.0));
    }
    for (const char* s : {"", "-wal", "-shm"}) std::remove((db + s).c_str());
}

// ---------------------------------------------------------------------------
// WHICH BROKER PLACED THE ORDERS — the field that separates a real session from
// a rehearsal, and the field that was wrong for 107 consecutive sessions.

TEST_CASE("journal mode: the TWS route is a REAL broker, not a simulation") {
    // THE DEFECT, found on 2026-08-20 by reading the day's journal. The call
    // site was `ibkr_ ? "ibkr" : "sim"`, written when the Client-Portal gateway
    // was the only real route. The TWS socket route is the VPS's ONLY route and
    // leaves ibkr_ null, so every live session since 2026-07-08 was journalled
    // as a SIMULATION while placing real orders against the paper account.
    CHECK(std::string(tt::ui::live_broker_mode(true, false)) == "tws");
    CHECK(std::string(tt::ui::live_broker_mode(false, true)) == "ibkr");
    // Only the absence of BOTH is a simulation.
    CHECK(std::string(tt::ui::live_broker_mode(false, false)) == "sim");
    // Belt and braces: a real broker must never report "sim", whichever it is.
    for (int t = 0; t < 2; ++t)
        for (int i = 0; i < 2; ++i)
            if (t || i)
                CHECK(std::string(tt::ui::live_broker_mode(t != 0, i != 0)) != "sim");
}

TEST_CASE("journal mode: the call site asks about BOTH brokers") {
    // SOURCE-TEXT PIN. Nothing in the suite constructs an App, so the function
    // above can be right while the call site still ignores tws_ — which is
    // precisely the state this defect lived in for six weeks, one line above a
    // statement that DID handle tws_ correctly.
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    CHECK(src.find("ibkr_ ? \"ibkr\" : \"sim\"") == std::string::npos);
    CHECK(src.find("live_broker_mode(tws_ != nullptr, ibkr_ != nullptr)") !=
          std::string::npos);
}
