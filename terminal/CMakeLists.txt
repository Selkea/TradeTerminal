set(TT_REPO_ROOT ${CMAKE_SOURCE_DIR})
configure_file(src/dev_paths.h.in ${CMAKE_CURRENT_BINARY_DIR}/dev_paths.h @ONLY)

add_executable(tt_terminal
    src/main.cpp
    src/app.cpp
    src/candle_plot.cpp
    src/net/ipc_client.cpp
    src/panels/log_console.cpp
    src/panels/chart.cpp
    src/panels/watchlist.cpp
    src/panels/backtest.cpp
    src/panels/strategy_mgr.cpp
)
target_include_directories(tt_terminal PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(tt_terminal PRIVATE
    tt_engine
    imgui
    implot
    nlohmann_json
    opengl32
    ws2_32
)
