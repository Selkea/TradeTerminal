set(TT_REPO_ROOT ${CMAKE_SOURCE_DIR})
find_package(CURL REQUIRED)   # alert webhook POSTs
configure_file(src/dev_paths.h.in ${CMAKE_CURRENT_BINARY_DIR}/dev_paths.h @ONLY)

# Stamp the git commit into build_info.h at build time (not configure time) so
# /diag always reports the exact source running on the VPS. The custom target
# reruns git each build; configure_file only rewrites the header when the commit
# actually moved, so it doesn't churn the build.
set(TT_BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(tt_build_info
    BYPRODUCTS ${TT_BUILD_INFO}
    COMMAND ${CMAKE_COMMAND}
        -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.h.in
        -DOUT=${TT_BUILD_INFO}
        -DGIT_SRC=${TT_REPO_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_commit.cmake
    COMMENT "Stamping git commit into build_info.h"
    VERBATIM)

add_executable(tt_terminal
    src/main.cpp
    src/account_store.cpp
    src/alerts.cpp
    src/app.cpp
    src/config.cpp
    src/journal.cpp
    src/panels/journal_panel.cpp
    src/candle_plot.cpp
    src/net/gateway_data.cpp
    src/net/tws_data.cpp
    src/net/diag_server.cpp
    src/panels/log_console.cpp
    src/panels/chart.cpp
    src/panels/watchlist.cpp
    src/panels/backtest.cpp
    src/panels/replay.cpp
    src/panels/strategy_mgr.cpp
    src/panels/sweep.cpp
    src/panels/trade.cpp
    src/panels/blotter.cpp
    src/panels/positions.cpp
)
if(WIN32)
    target_sources(tt_terminal PRIVATE resources/app.rc)
    # windres doesn't emit a depfile, so ninja can't see that app.rc embeds
    # app.ico -- without this, changing the icon silently relinks the old one.
    set_source_files_properties(resources/app.rc PROPERTIES OBJECT_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico)
endif()

# Strategies compiled directly into tt_terminal instead of hot-loaded as DLLs
# ("promoted"). Same .cpp as strategies/*.cpp, byte-for-byte unmodified --
# TT_STRATEGY_STATIC_LINK switches TT_STRATEGY (sdk/include/tt/strategy_api.h)
# to self-registration instead of dllexport. TT_STRATEGY_STATIC_KEY must match
# the source's basename exactly (the string StrategyHost would key it under if
# hot-built) -- every other integration point (config.json, autopilot, panel
# dropdowns) treats that string as the strategy's identity.
#
# CAUTION: a promoted strategy's C++ class name must be globally unique across
# every OTHER promoted strategy (see strategies/README.md) -- unlike DLLs,
# promoted strategies share one link unit, and a name collision is a silent
# ODR violation (wrong strategy logic quietly runs under one of the keys, no
# compiler/linker diagnostic), not a build error. Guarded below at configure
# time.
# sma_crossover.cpp is included here too: it's the app's "" built-in (see
# App::acquire_strategy) -- promoting it lets "" resolve to this ONE
# implementation instead of a separately-hand-maintained duplicate class
# (engine/include/engine/builtin_sma.h, removed -- it had drifted from this
# file: no in-flight order-id guard, no buying-power cap).
set(TT_PROMOTED_STRATEGIES
    ${CMAKE_SOURCE_DIR}/strategies/orb_breakout.cpp
    ${CMAKE_SOURCE_DIR}/strategies/donchian_trend.cpp
    ${CMAKE_SOURCE_DIR}/strategies/rsi2_pullback.cpp
    ${CMAKE_SOURCE_DIR}/strategies/bollinger_reversion.cpp
    ${CMAKE_SOURCE_DIR}/strategies/scalper_burst.cpp
    ${CMAKE_SOURCE_DIR}/strategies/sma_crossover.cpp
)
target_sources(tt_terminal PRIVATE ${TT_PROMOTED_STRATEGIES})
set(_tt_promoted_classes)
foreach(_src ${TT_PROMOTED_STRATEGIES})
    get_filename_component(_key ${_src} NAME)
    set_source_files_properties(${_src} PROPERTIES
        COMPILE_DEFINITIONS "TT_STRATEGY_STATIC_LINK;TT_STRATEGY_STATIC_KEY=\"${_key}\"")

    file(STRINGS ${_src} _class_line REGEX "^class [A-Za-z_][A-Za-z0-9_]* " LIMIT_COUNT 1)
    if(_class_line)
        string(REGEX MATCH "^class ([A-Za-z_][A-Za-z0-9_]*)" _unused "${_class_line}")
        set(_cls ${CMAKE_MATCH_1})
        list(FIND _tt_promoted_classes ${_cls} _idx)
        if(NOT _idx EQUAL -1)
            message(FATAL_ERROR "Promoted strategy class name collision: '${_cls}' "
                "(from ${_src}) is already used by another TT_PROMOTED_STRATEGIES source. "
                "Promoted strategies share one link unit, so this is a silent ODR "
                "violation, not a build error -- rename one of the classes.")
        endif()
        list(APPEND _tt_promoted_classes ${_cls})
    endif()
endforeach()

add_dependencies(tt_terminal tt_build_info)   # generate build_info.h before compiling
target_include_directories(tt_terminal PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
# Debug-only features (Debug menu, tick simulator) compile out of release.
target_compile_definitions(tt_terminal PRIVATE $<$<CONFIG:Debug>:TT_DEBUG>)
target_link_libraries(tt_terminal PRIVATE
    tt_engine
    tt_net
    imgui
    implot
    nlohmann_json
    opengl32
    ws2_32
    crypt32     # DPAPI (account_store)
    winmm       # timeBeginPeriod (1 ms timer granularity)
    CURL::libcurl   # alert webhook POSTs
    sqlite3     # trade journal (MSYS2 package)
)
if(WIN32)
    target_link_libraries(tt_terminal PRIVATE dwmapi)
endif()

# Windowed (no console) in every config — the app logs to its own Log Console
# panel, so the attached console was just a blank window. Under a debugger,
# stdout/stderr are captured by the IDE regardless of subsystem.
set_target_properties(tt_terminal PROPERTIES WIN32_EXECUTABLE TRUE)

# Terminal-side unit tests. LogConsole pulls in imgui (its filter widget), so
# this can't live in the engine test harness; it links imgui like the app does.
add_executable(tt_terminal_tests
    tests/test_log_console.cpp
    src/panels/log_console.cpp
)
target_include_directories(tt_terminal_tests PRIVATE src)
target_link_libraries(tt_terminal_tests PRIVATE imgui doctest)
add_test(NAME terminal_tests COMMAND tt_terminal_tests)
