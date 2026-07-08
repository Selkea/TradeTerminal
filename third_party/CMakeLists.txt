find_package(glfw3 REQUIRED)

add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC glfw)
target_compile_options(imgui PRIVATE -w)

add_library(implot STATIC
    implot/implot.cpp
    implot/implot_items.cpp
    implot/implot_demo.cpp
)
target_include_directories(implot PUBLIC implot)
target_link_libraries(implot PUBLIC imgui)
target_compile_options(implot PRIVATE -w)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE doctest)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# SIMD JSON parsing for the market-data hot path (amalgamated release build).
add_library(simdjson STATIC simdjson/simdjson.cpp)
target_include_directories(simdjson PUBLIC simdjson)
target_compile_options(simdjson PRIVATE -w)

# ---- IBKR TWS API (socket route) -------------------------------------------
# Downloaded at configure time: the sources are under the IB API Non-Commercial
# License and are NOT redistributed in this repository. The Intel Decimal
# library dependency is satisfied by tws_decimal_shim.cpp (doubles under the
# hood), so no extra downloads are needed.
include(FetchContent)
FetchContent_Declare(twsapi_src
    URL https://interactivebrokers.github.io/downloads/twsapi_macunix.1030.01.zip
    URL_HASH SHA256=ea79fa5b4c7b30359458424085e55f918115a2889efc248a2b79319da71a139a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(twsapi_src)
file(GLOB TWSAPI_SOURCES ${twsapi_src_SOURCE_DIR}/IBJts/source/cppclient/client/*.cpp)
add_library(twsapi STATIC ${TWSAPI_SOURCES} tws_decimal_shim.cpp)
target_include_directories(twsapi PUBLIC
    ${twsapi_src_SOURCE_DIR}/IBJts/source/cppclient/client)
target_link_libraries(twsapi PUBLIC ws2_32)
target_compile_options(twsapi PRIVATE -w)
