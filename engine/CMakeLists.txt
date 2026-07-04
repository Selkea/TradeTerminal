add_library(tt_engine STATIC
    src/version.cpp
    src/clock.cpp
    src/portfolio.cpp
    src/exec_sim.cpp
    src/engine.cpp
    src/strategy_host.cpp
)
target_include_directories(tt_engine PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/sdk/include
)

add_executable(tt_engine_tests
    tests/test_spsc_ring.cpp
    tests/test_engine.cpp
    tests/test_strategy_host.cpp
)
target_link_libraries(tt_engine_tests PRIVATE tt_engine doctest)
target_compile_definitions(tt_engine_tests PRIVATE
    TT_TEST_SDK_INCLUDE="${CMAKE_SOURCE_DIR}/sdk/include"
    TT_TEST_STRATEGY_SRC="${CMAKE_SOURCE_DIR}/strategies/sma_crossover.cpp"
)
add_test(NAME engine_tests COMMAND tt_engine_tests)
