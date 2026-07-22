#include "doctest.h"

#include "engine/engine.h"
#include "tt/strategy_registry.h"

#include <chrono>
#include <cmath>
#include <thread>

// Regression guard for the TT_STRATEGY_STATIC_LINK branch of the TT_STRATEGY
// macro (sdk/include/tt/strategy_api.h): proves the "promoted" (statically-
// linked) path works, using the exact same source the DLL-mode tests in
// test_strategy_host.cpp already build via StrategyHost::compile() — one file
// verified to behave identically both ways. See engine/CMakeLists.txt for how
// strategies/sma_crossover.cpp gets compiled a second time, as a static-link
// source, into this test binary.

using namespace tt;

namespace {
BacktestConfig make_cfg(const char* symbol) {
    std::vector<Bar> bars;
    const int64_t day = 86'400'000'000'000;
    double px = 100.0;
    for (int i = 0; i < 300; ++i) {
        px = std::max(5.0, px + std::sin(i / 12.0) * 1.5);
        bars.push_back(Bar{int64_t{1'600'000'000'000'000'000} + i * day,
                           px - 0.2, px + 0.5, px - 0.5, px, 1e6});
    }
    BacktestConfig cfg;
    cfg.symbol = symbol;
    cfg.bars = std::move(bars);
    cfg.params = {{"fast", 5}, {"slow", 15}, {"qty", 50}};
    return cfg;
}

BacktestResult run_backtest(Engine& eng, const BacktestConfig& cfg, IStrategy* s) {
    REQUIRE(eng.start_backtest(cfg, s));
    BacktestResult res;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!eng.take_result(res)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return res;
}
} // namespace

TEST_CASE("static strategy registry: sma_crossover.cpp self-registers") {
    const StaticStrategyEntry* e = find_static_strategy("sma_crossover.cpp");
    REQUIRE(e != nullptr);
    CHECK(std::string(e->info->name) == "SMA Crossover");
    REQUIRE(e->info->param_count == 3);
    CHECK(std::string(e->info->params[0].name) == "fast");
}

TEST_CASE("static strategy registry: create() runs a real backtest") {
    const StaticStrategyEntry* e = find_static_strategy("sma_crossover.cpp");
    REQUIRE(e != nullptr);
    IStrategy* inst = e->create();
    REQUIRE(inst != nullptr);
    Engine eng;
    const BacktestResult res = run_backtest(eng, make_cfg("STATICTEST"), inst);
    CHECK(res.trades > 2);
    CHECK(res.final_equity > 0.0);
    inst->destroy();
}

TEST_CASE("static strategy registry: unknown key returns nullptr") {
    CHECK(find_static_strategy("does_not_exist.cpp") == nullptr);
}
