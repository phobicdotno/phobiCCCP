cmake_minimum_required(VERSION 3.19)
project(phobicCC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)
find_package(ZLIB REQUIRED)

add_executable(phobiccc
    src/main.cpp
    src/zlibutil.cpp
    src/c2ddocument.cpp
    src/element.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/post_grbl.cpp
)

target_link_libraries(phobiccc PRIVATE Qt6::Widgets Qt6::Sql ZLIB::ZLIB)

install(TARGETS phobiccc RUNTIME DESTINATION bin)

# Headless geometry/round-trip checks: -DPHOBICCC_TESTS=ON, then run
# build/test_geometry [sample.c2d]
option(PHOBICCC_TESTS "Build the headless test binary" OFF)
if(PHOBICCC_TESTS)
    add_executable(test_geometry
        tests/test_geometry.cpp
        src/zlibutil.cpp
        src/c2ddocument.cpp
        src/element.cpp
    )
    target_link_libraries(test_geometry PRIVATE Qt6::Gui Qt6::Sql ZLIB::ZLIB)
endif()
