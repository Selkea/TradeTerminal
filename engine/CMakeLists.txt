add_library(tt_engine STATIC
    src/version.cpp
    src/clock.cpp
    src/portfolio.cpp
    src/exec_sim.cpp
    src/engine.cpp
)
target_include_directories(tt_engine PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/sdk/include
)

add_executable(tt_engine_tests
    tests/test_spsc_ring.cpp
    tests/test_engine.cpp
)
target_link_libraries(tt_engine_tests PRIVATE tt_engine doctest)
add_test(NAME engine_tests COMMAND tt_engine_tests)
