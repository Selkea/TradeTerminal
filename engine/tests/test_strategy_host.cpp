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

std::string key_of(const std::string& src) {
    return fs::path(src).filename().string();
}

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

TEST_CASE("strategy host: compile, load, run, hot-reload x5") {
    StrategyHost host(TT_TEST_GXX, TT_TEST_SDK_INCLUDE, out_dir());
    host.sweep_stale();
    const std::string key = key_of(TT_TEST_STRATEGY_SRC);

    std::string last_output;
    auto on_out = [&](std::string l) { last_output = std::move(l); };

    // Five consecutive build->load cycles: proves versioned DLLs dodge the
    // Windows file lock, and that replacing an idle module frees it.
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::string dll;
        REQUIRE_MESSAGE(host.compile(TT_TEST_STRATEGY_SRC, on_out, dll),
                        "compile failed: ", last_output);
        std::string err;
        REQUIRE_MESSAGE(host.load(dll, TT_TEST_STRATEGY_SRC, err), err);
        REQUIRE(host.has(key));
        StrategyHost::ModuleView mv;
        REQUIRE(host.info(key, mv));
        CHECK(mv.name == std::string("SMA Crossover"));
        REQUIRE(mv.params.size() == 3);
        CHECK(mv.params[0].name == "fast");
        CHECK(mv.instances == 0);
    }
    CHECK_FALSE(host.stale(key));

    // Run a real backtest through a per-run DLL instance.
    IStrategy* inst = host.create_instance(key);
    REQUIRE(inst != nullptr);
    Engine eng;
    const BacktestResult res = run_backtest(eng, make_cfg("DLLTEST"), inst);
    CHECK(res.trades > 2);
    CHECK(res.final_equity > 0.0);
    host.destroy_instance(inst);

    host.unload(key);
    CHECK_FALSE(host.has(key));
}

TEST_CASE("strategy host: instances outlive replacement and unload") {
    StrategyHost host(TT_TEST_GXX, TT_TEST_SDK_INCLUDE, out_dir());
    host.sweep_stale();
    const std::string key = key_of(TT_TEST_STRATEGY_SRC);

    std::string dll_v1;
    REQUIRE(host.compile(TT_TEST_STRATEGY_SRC, nullptr, dll_v1));
    std::string err;
    REQUIRE_MESSAGE(host.load(dll_v1, TT_TEST_STRATEGY_SRC, err), err);

    // Two live handles from v1: independent objects, refcount visible.
    IStrategy* a = host.create_instance(key);
    IStrategy* b = host.create_instance(key);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    StrategyHost::ModuleView mv;
    REQUIRE(host.info(key, mv));
    CHECK(mv.instances == 2);

    // Replace the module while v1 instances are outstanding: v1's DLL must
    // stay mapped (the instances keep working) even though the module table
    // now serves v2.
    std::string dll_v2;
    REQUIRE(host.compile(TT_TEST_STRATEGY_SRC, nullptr, dll_v2));
    REQUIRE_MESSAGE(host.load(dll_v2, TT_TEST_STRATEGY_SRC, err), err);
    REQUIRE(host.info(key, mv));
    CHECK(mv.dll_path == dll_v2);
    CHECK(mv.instances == 0);          // v1's handles belong to the retired module
    CHECK(fs::exists(dll_v1));         // still mapped: retired, not freed

    // The retired instance still runs a full backtest.
    Engine eng;
    const BacktestResult res = run_backtest(eng, make_cfg("RETIRED"), a);
    CHECK(res.final_equity > 0.0);

    // Last v1 handle released -> retired module freed, its DLL deleted.
    host.destroy_instance(a);
    CHECK(fs::exists(dll_v1));
    host.destroy_instance(b);
    CHECK_FALSE(fs::exists(dll_v1));

    // Unload with an outstanding instance: same deferred-free contract.
    IStrategy* c = host.create_instance(key);
    REQUIRE(c != nullptr);
    host.unload(key);
    CHECK_FALSE(host.has(key));
    CHECK(fs::exists(dll_v2));
    const BacktestResult res2 = run_backtest(eng, make_cfg("UNLOADED"), c);
    CHECK(res2.final_equity > 0.0);
    host.destroy_instance(c);
    CHECK_FALSE(fs::exists(dll_v2));
}

TEST_CASE("strategy host: two different modules loaded side by side") {
    StrategyHost host(TT_TEST_GXX, TT_TEST_SDK_INCLUDE, out_dir());
    host.sweep_stale();

    // A second, minimal strategy source written on the fly.
    const std::string mini_src = (fs::path(out_dir()) / "mini.cpp").string();
    {
        FILE* f = std::fopen(mini_src.c_str(), "w");
        REQUIRE(f);
        std::fputs(
            "#include \"tt/strategy_api.h\"\n"
            "using namespace tt;\n"
            "namespace { constexpr ParamDesc kP[] = {{\"x\", 1, 0, 10}}; }\n"
            "class Mini final : public IStrategy {\n"
            "    void on_init(IStrategyContext&) noexcept override {}\n"
            "    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}\n"
            "    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}\n"
            "    void on_fill(IStrategyContext&, const Fill&) noexcept override {}\n"
            "    void on_stop(IStrategyContext&) noexcept override {}\n"
            "    void destroy() noexcept override { delete this; }\n"
            "};\n"
            "TT_STRATEGY(Mini, \"Mini\", kP)\n",
            f);
        std::fclose(f);
    }

    std::string dll_sma, dll_mini, err;
    REQUIRE(host.compile(TT_TEST_STRATEGY_SRC, nullptr, dll_sma));
    REQUIRE(host.compile(mini_src, nullptr, dll_mini));
    REQUIRE_MESSAGE(host.load(dll_sma, TT_TEST_STRATEGY_SRC, err), err);
    REQUIRE_MESSAGE(host.load(dll_mini, mini_src, err), err);

    const std::string sma_key = key_of(TT_TEST_STRATEGY_SRC);
    REQUIRE(host.has(sma_key));
    REQUIRE(host.has("mini.cpp"));
    CHECK(host.modules().size() == 2);

    // Instances from both modules coexist — the "backtest B while A runs"
    // shape, minus the engine threads.
    IStrategy* a = host.create_instance(sma_key);
    IStrategy* b = host.create_instance("mini.cpp");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    Engine eng;
    CHECK(run_backtest(eng, make_cfg("SIDEBYSIDE"), a).final_equity > 0.0);
    host.destroy_instance(a);
    host.destroy_instance(b);
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
    CHECK_FALSE(host.has("broken.cpp"));
}
