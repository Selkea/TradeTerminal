cmake_minimum_required(VERSION 3.25)
# Single source of truth for the semantic version. Bump the VERSION file on
# every commit: major = full release, minor = moderate change, patch = small.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TT_VERSION_RAW)
string(STRIP "${TT_VERSION_RAW}" TT_VERSION_RAW)
project(TradeTerminal VERSION ${TT_VERSION_RAW} LANGUAGES CXX)
# Reconfigure when VERSION changes so PROJECT_VERSION (and the stamped
# TT_VERSION_BASE) stay current without a manual cmake rerun.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
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
