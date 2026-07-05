cmake_minimum_required(VERSION 3.25)
project(TradeTerminal LANGUAGES CXX)
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
