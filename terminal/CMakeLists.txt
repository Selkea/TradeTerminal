add_executable(tt_terminal
    src/main.cpp
    src/app.cpp
    src/panels/log_console.cpp
)
target_include_directories(tt_terminal PRIVATE src)
target_link_libraries(tt_terminal PRIVATE
    tt_engine
    imgui
    implot
    opengl32
)
