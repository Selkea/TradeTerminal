set(TT_REPO_ROOT ${CMAKE_SOURCE_DIR})
find_package(CURL REQUIRED)   # alert webhook POSTs
configure_file(src/dev_paths.h.in ${CMAKE_CURRENT_BINARY_DIR}/dev_paths.h @ONLY)

add_executable(tt_terminal
    src/main.cpp
    src/account_store.cpp
    src/alerts.cpp
    src/app.cpp
    src/config.cpp
    src/journal.cpp
    src/panels/journal_panel.cpp
    src/candle_plot.cpp
    src/net/ipc_client.cpp
    src/panels/log_console.cpp
    src/panels/chart.cpp
    src/panels/watchlist.cpp
    src/panels/backtest.cpp
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
target_include_directories(tt_terminal PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
# Debug-only features (Debug menu, tick simulator) compile out of release.
target_compile_definitions(tt_terminal PRIVATE $<$<CONFIG:Debug>:TT_DEBUG>)
target_link_libraries(tt_terminal PRIVATE
    tt_engine
    tt_alpaca
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

# Release builds are windowed (no console); debug keeps the console for logs.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(tt_terminal PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
