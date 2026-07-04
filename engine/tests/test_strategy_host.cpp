#include "doctest.h"

#include "engine/engine.h"
#include "engine/strategy_host.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

// Paths injected by CMake (absolute, this repo).
#ifndef TT_TEST_GXX
#define TT_TEST_GXX "C:/msys64/ucrt64/bin/g++.exe"
#endif

using namespace tt;
namespace fs = std::filesystem;

namespace {
std::string out_dir() {
    auto d = fs::temp_directory_path() / "tt_host_test";
    fs::create_directories(d);
    return d.string();
}
} // namespace

TEST_CASE("strategy host: compile, load, run, hot-reload x5") {
    StrategyHost host(TT_TEST_GXX, TT_TEST_SDK_INCLUDE, out_dir());
    host.sweep_stale();

    std::string last_output;
    auto on_out = [&](std::string l) { last_output = std::move(l); };

    // Five consecutive build->load cycles: proves versioned DLLs dodge the
    // Windows file lock on loaded modules.
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::string dll;
        REQUIRE_MESSAGE(host.compile(TT_TEST_STRATEGY_SRC, on_out, dll),
                        "compile failed: ", last_output);
        std::string err;
        REQUIRE_MESSAGE(host.load(dll, TT_TEST_STRATEGY_SRC, err), err);
        REQUIRE(host.current() != nullptr);
        CHECK(host.current()->name == std::string("SMA Crossover"));
        REQUIRE(host.current()->params.size() == 3);
        CHECK(host.current()->params[0].name == "fast");
    }

    // Run a real backtest through the DLL-loaded strategy.
    std::vector<Bar> bars;
    const int64_t day = 86'400'000'000'000;
    double px = 100.0;
    for (int i = 0; i < 300; ++i) {
        px = std::max(5.0, px + std::sin(i / 12.0) * 1.5);
        bars.push_back(Bar{int64_t{1'600'000'000'000'000'000} + i * day,
                           px - 0.2, px + 0.5, px - 0.5, px, 1e6});
    }
    BacktestConfig cfg;
    cfg.symbol = "DLLTEST";
    cfg.bars = std::move(bars);
    cfg.params = {{"fast", 5}, {"slow", 15}, {"qty", 50}};

    Engine eng;
    REQUIRE(eng.start_backtest(cfg, host.current()->instance));
    BacktestResult res;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!eng.take_result(res)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(res.trades > 2);
    CHECK(res.final_equity > 0.0);

    host.unload();
    CHECK(host.current() == nullptr);
}

TEST_CASE("strategy host: broken source reports compiler errors, no load") {
    const std::string bad_src = (fs::path(out_dir()) / "broken.cpp").string();
    {
        FILE* f = std::fopen(bad_src.c_str(), "w");
        REQUIRE(f);
        std::fputs("#include \"tt/strategy_api.h\"\nthis is not C++\n", f);
        std::fclose(f);
    }
    StrategyHost host(TT_TEST_GXX, TT_TEST_SDK_INCLUDE, out_dir());
    bool saw_error_line = false;
    std::string dll;
    const bool ok = host.compile(
        bad_src,
        [&](std::string l) {
            if (l.find("error") != std::string::npos) saw_error_line = true;
        },
        dll);
    CHECK_FALSE(ok);
    CHECK(saw_error_line);
    CHECK(host.current() == nullptr);
}
