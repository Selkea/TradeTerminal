// .ttk round-trip and deterministic replay.
#include "doctest.h"

#include "engine/engine.h"
#include "engine/tick_log.h"
#include "tt/strategy_registry.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace tt;

namespace {

EngineEvent tick(uint32_t sid, int64_t ts_ns, double px) {
    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::Tick);
    ev.symbol_id = sid;
    ev.ts_event_ns = ts_ns;
    ev.u.tick.price = px;
    ev.u.tick.bid = px;
    ev.u.tick.ask = px;
    return ev;
}

// Sine wave around 100: guarantees SMA crossovers, hence fills.
TickLog make_log() {
    TickLog log;
    log.symbols = {"AAPL", "MSFT"};
    log.bar_seconds = 60;
    const int64_t t0 = 1'700'000'000'000'000'000LL;
    for (int i = 0; i < 480; ++i) {
        const int64_t ts = t0 + static_cast<int64_t>(i) * 15'000'000'000LL;
        const double px = 100.0 + 5.0 * std::sin(i / 40.0);
        log.events.push_back(tick(1, ts, px));
        log.events.push_back(tick(2, ts, 2.0 * px));
    }
    return log;
}

BacktestResult replay_once(Engine& eng, const TickLog& log) {
    // sma_crossover.cpp is compiled into this test binary as a static-link
    // source (see engine/CMakeLists.txt) -- the same implementation
    // tt_terminal's "" built-in resolves to (see App::acquire_strategy).
    const StaticStrategyEntry* e = find_static_strategy("sma_crossover.cpp");
    REQUIRE(e != nullptr);
    IStrategy* strat = e->create();

    ReplayConfig cfg;
    cfg.name = "replay:test";
    cfg.log = log;
    cfg.params = {{"fast", 5.0}, {"slow", 20.0}};
    REQUIRE(eng.start_replay(std::move(cfg), strat));
    while (eng.running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BacktestResult res;
    REQUIRE(eng.take_result(res));
    strat->destroy();
    return res;
}

} // namespace

TEST_CASE("tick log round-trips through disk") {
    const TickLog log = make_log();
    const char* path = "test_ticks.ttk";

    TickLogWriter w;
    REQUIRE(w.open(path, log.symbols, log.bar_seconds));
    for (const EngineEvent& ev : log.events) w.write(ev);
    CHECK(w.written() == log.events.size());
    w.close();

    TickLog rd;
    std::string err;
    REQUIRE(tick_log_read(path, rd, err));
    CHECK(rd.symbols == log.symbols);
    CHECK(rd.bar_seconds == log.bar_seconds);
    REQUIRE(rd.events.size() == log.events.size());
    CHECK(rd.events.front().ts_event_ns == log.events.front().ts_event_ns);
    CHECK(rd.events.back().u.tick.price ==
          doctest::Approx(log.events.back().u.tick.price));
    std::remove(path);
}

TEST_CASE("tick_log_read rejects junk") {
    const char* path = "test_junk.ttk";
    FILE* f = std::fopen(path, "wb");
    std::fwrite("not a tick log at all", 21, 1, f);
    std::fclose(f);
    TickLog rd;
    std::string err;
    CHECK_FALSE(tick_log_read(path, rd, err));
    CHECK_FALSE(err.empty());
    std::remove(path);
}

TEST_CASE("replay is deterministic and actually trades") {
    Engine eng;
    const TickLog log = make_log();
    const BacktestResult a = replay_once(eng, log);
    const BacktestResult b = replay_once(eng, log);

    CHECK(a.trades > 0);                      // the sine wave forces crossovers
    CHECK(a.events == 2 * 480);
    CHECK(a.final_equity == b.final_equity);  // bit-identical, not Approx
    CHECK(a.trades == b.trades);
    CHECK(a.max_drawdown == b.max_drawdown);
    REQUIRE(a.fills.size() == b.fills.size());
    for (size_t i = 0; i < a.fills.size(); ++i) {
        CHECK(a.fills[i].price == b.fills[i].price);
        CHECK(a.fills[i].ts_ns == b.fills[i].ts_ns);
    }
}
