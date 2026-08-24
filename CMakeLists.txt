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
    src/propertiespanel.cpp
    src/toolpathpanel.cpp
    src/post_grbl.cpp
)

target_link_libraries(phobiccc PRIVATE Qt6::Widgets Qt6::Sql ZLIB::ZLIB)

install(TARGETS phobiccc RUNTIME DESTINATION bin)
