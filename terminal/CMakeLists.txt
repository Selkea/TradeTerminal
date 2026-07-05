set(TT_REPO_ROOT ${CMAKE_SOURCE_DIR})
configure_file(src/dev_paths.h.in ${CMAKE_CURRENT_BINARY_DIR}/dev_paths.h @ONLY)

add_executable(tt_terminal
    src/main.cpp
    src/app.cpp
    src/config.cpp
    src/candle_plot.cpp
    src/net/ipc_client.cpp
    src/panels/log_console.cpp
    src/panels/chart.cpp
    src/panels/watchlist.cpp
    src/panels/backtest.cpp
    src/panels/strategy_mgr.cpp
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
target_link_libraries(tt_terminal PRIVATE
    tt_engine
    imgui
    implot
    nlohmann_json
    opengl32
    ws2_32
)

# Release builds are windowed (no console); debug keeps the console for logs.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(tt_terminal PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
