cmake_minimum_required(VERSION 3.25)
project(TradeTerminal VERSION 0.1.0 LANGUAGES CXX)   # bump here to cut a release; build number auto-tracks commits
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(terminal)
