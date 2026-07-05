add_library(tt_engine STATIC
    src/version.cpp
    src/clock.cpp
    src/portfolio.cpp
    src/exec_sim.cpp
    src/engine.cpp
    src/strategy_host.cpp
    src/tick_log.cpp
)
target_include_directories(tt_engine PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/sdk/include
)

# Alpaca broker adapter — its own lib so tt_engine (and the engine tests)
# stay free of libcurl and networking.
find_package(CURL REQUIRED)
add_library(tt_alpaca STATIC
    src/alpaca_broker.cpp
    src/alpaca_feed.cpp
)
target_link_libraries(tt_alpaca PUBLIC tt_engine PRIVATE CURL::libcurl nlohmann_json simdjson)

add_executable(tt_engine_tests
    tests/test_spsc_ring.cpp
    tests/test_engine.cpp
    tests/test_strategy_host.cpp
    tests/test_tick_log.cpp
    tests/test_exec_brackets.cpp
)
target_link_libraries(tt_engine_tests PRIVATE tt_engine doctest)
target_compile_definitions(tt_engine_tests PRIVATE
    TT_TEST_SDK_INCLUDE="${CMAKE_SOURCE_DIR}/sdk/include"
    TT_TEST_STRATEGY_SRC="${CMAKE_SOURCE_DIR}/strategies/sma_crossover.cpp"
)
add_test(NAME engine_tests COMMAND tt_engine_tests)

add_executable(tt_alpaca_tests tests/test_alpaca_parse.cpp)
target_link_libraries(tt_alpaca_tests PRIVATE tt_alpaca doctest)
add_test(NAME alpaca_tests COMMAND tt_alpaca_tests)

add_executable(tt_alpaca_feed_tests tests/test_alpaca_feed_parse.cpp)
target_link_libraries(tt_alpaca_feed_tests PRIVATE tt_alpaca doctest)
add_test(NAME alpaca_feed_tests COMMAND tt_alpaca_feed_tests)
