cmake_minimum_required(VERSION 3.19)
project(phobicCC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql SerialPort)
find_package(ZLIB REQUIRED)

# Exact polygon offsetting for CAM (pockets, contour offsets, v-carve rings).
# ON: fetch Clipper2 (Boost-1.0 license) at configure time; OFF: fall back to
# the approximate Qt path-boolean offsetting.
option(PHOBICC_WITH_CLIPPER2 "Use Clipper2 for exact toolpath offsetting" ON)
if(PHOBICC_WITH_CLIPPER2)
    include(FetchContent)
    FetchContent_Declare(clipper2
        GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR CPP)
    set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_USINGZ OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(clipper2)
endif()

add_executable(phobiccc
    src/main.cpp
    src/zlibutil.cpp
    src/c2ddocument.cpp
    src/element.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/propertiespanel.cpp
    src/toolpathpanel.cpp
    src/gcodeexport.cpp
    src/post_grbl.cpp
    src/grblstreamer.cpp
    src/machinepanel.cpp
)

target_link_libraries(phobiccc PRIVATE Qt6::Widgets Qt6::Sql Qt6::SerialPort ZLIB::ZLIB)
if(PHOBICC_WITH_CLIPPER2)
    target_link_libraries(phobiccc PRIVATE Clipper2)
    target_compile_definitions(phobiccc PRIVATE HAVE_CLIPPER2)
endif()

install(TARGETS phobiccc RUNTIME DESTINATION bin)
