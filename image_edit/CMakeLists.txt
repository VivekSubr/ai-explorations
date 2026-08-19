cmake_minimum_required(VERSION 3.21)

project(SvgPlayground VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)

qt_add_executable(svg-playground
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/svgcanvas.cpp
    src/svgcanvas.h
)

target_link_libraries(svg-playground PRIVATE Qt6::Widgets Qt6::Svg)

set_target_properties(svg-playground PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "SVG Playground"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.svgplayground.app"
)

install(TARGETS svg-playground
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
