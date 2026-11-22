cmake_minimum_required(VERSION 3.20)
project(NekoWaShock CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(NekoWaShock SHARED src/NekoWaShock.cpp res/NekoWaShock.def)
set_target_properties(NekoWaShock PROPERTIES
    PREFIX ""
    OUTPUT_NAME "NekoWaShock"
)
target_compile_options(NekoWaShock PRIVATE /O2 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
target_link_libraries(NekoWaShock PRIVATE user32 gdi32)
