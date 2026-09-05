cmake_minimum_required(VERSION 3.19)
project(phobiCCCP VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql SerialPort)
find_package(ZLIB REQUIRED)

# Exact polygon offsetting for CAM (pockets, contour offsets, v-carve rings).
# ON: fetch Clipper2 (Boost-1.0 license) at configure time; OFF: fall back to
# the approximate Qt path-boolean offsetting.
option(PHOBICCCP_WITH_CLIPPER2 "Use Clipper2 for exact toolpath offsetting" ON)
if(PHOBICCCP_WITH_CLIPPER2)
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

# Everything that is not a widget: file format, CAM, post, GRBL streaming.
# Shared by the app and the headless tests.
add_library(phobicccp_core STATIC
    src/zlibutil.cpp
    src/c2ddocument.cpp
    src/element.cpp
    src/gcodeexport.cpp
    src/vcarve.cpp
    src/post_grbl.cpp
    src/grblstreamer.cpp
    src/heightmodel.cpp
    src/simulation.cpp
    src/vectorops.cpp
    src/importers.cpp
    src/toollibrary.cpp
    src/tiling.cpp
    data/tools.qrc
    src/backgroundimage.cpp
    src/imagetrace.cpp
)
target_include_directories(phobicccp_core PUBLIC src)
target_link_libraries(phobicccp_core PUBLIC Qt6::Gui Qt6::Sql Qt6::SerialPort ZLIB::ZLIB)

add_executable(phobicccp
    src/main.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/propertiespanel.cpp
    src/toolpathpanel.cpp
    src/toollibrarydialog.cpp
    src/machinepanel.cpp
    src/isopreview.cpp
    src/simpanel.cpp
    src/vectoractions.cpp
    src/importui.cpp
    src/tracedialog.cpp
    src/backgrounddialog.cpp
)

target_link_libraries(phobicccp PRIVATE phobicccp_core Qt6::Widgets)
if(PHOBICCCP_WITH_CLIPPER2)
    target_link_libraries(phobicccp_core PUBLIC Clipper2)
    target_compile_definitions(phobicccp_core PUBLIC HAVE_CLIPPER2)
endif()

# True medial-axis v-carve needs the (header-only) Boost.Polygon Voronoi
# builder; without it the exporter falls back to depth-graded inset rings.
find_path(BOOST_VORONOI_INC boost/polygon/voronoi.hpp)
if(BOOST_VORONOI_INC)
    target_include_directories(phobicccp_core PRIVATE ${BOOST_VORONOI_INC})
    target_compile_definitions(phobicccp_core PUBLIC HAVE_BOOST_VORONOI)
    message(STATUS "Boost.Polygon Voronoi found: medial-axis v-carve enabled")
else()
    message(STATUS "Boost.Polygon Voronoi NOT found: v-carve uses ring fallback")
endif()

# Tests: `ctest --test-dir build`. Geometry/CAM checks are plain asserts;
# the GRBL simulator and the machine flow test need python3.
enable_testing()
add_executable(test_geometry tests/test_geometry.cpp)
target_link_libraries(test_geometry PRIVATE phobicccp_core)
add_test(NAME geometry COMMAND test_geometry)
add_executable(test_simulation tests/test_simulation.cpp)
target_link_libraries(test_simulation PRIVATE phobicccp_core)
add_test(NAME simulation COMMAND test_simulation)
add_executable(test_vectorops tests/test_vectorops.cpp)
target_link_libraries(test_vectorops PRIVATE phobicccp_core)
add_test(NAME vectorops COMMAND test_vectorops)
add_executable(test_import tests/test_import.cpp)
target_link_libraries(test_import PRIVATE phobicccp_core)
target_compile_definitions(test_import PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME import COMMAND test_import)
add_executable(test_tiling tests/test_tiling.cpp)
target_link_libraries(test_tiling PRIVATE phobicccp_core)
add_test(NAME tiling COMMAND test_tiling)
add_executable(test_trace tests/test_trace.cpp)
target_link_libraries(test_trace PRIVATE phobicccp_core)
add_test(NAME trace COMMAND test_trace)
add_executable(test_paths tests/test_paths.cpp)
target_link_libraries(test_paths PRIVATE phobicccp_core)
add_test(NAME paths COMMAND test_paths)
# Canvas interaction test (pen tool, node editor) drives the real widget with
# synthetic events; needs the optional Qt6 Test module.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    add_executable(test_canvas tests/test_canvas.cpp src/canvas.cpp)
    target_link_libraries(test_canvas PRIVATE phobicccp_core Qt6::Widgets Qt6::Test)
    add_test(NAME canvas COMMAND test_canvas)
else()
    message(STATUS "Qt6 Test not found: canvas interaction test skipped")
endif()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME grblsim COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/grblsim_selftest.py)
    add_test(NAME machine_flow COMMAND ${CMAKE_SOURCE_DIR}/tools/flowtest.sh $<TARGET_FILE:phobicccp>)
endif()

include(GNUInstallDirs)
install(TARGETS phobicccp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration: app-menu launcher, icon and .c2d file association, so
# `cmake --install` gives a start-menu entry and double-click-to-open, not just
# a binary. Works for both system (/usr/local) and user (~/.local) prefixes.
configure_file(desktop/phobicccp.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/phobicccp.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/phobicccp.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES desktop/phobicccp.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES desktop/phobicccp-c2d.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)

# Refresh the desktop/mime/icon caches after a real (non-packaging) install.
install(CODE [[
    if(NOT DEFINED ENV{DESTDIR} OR "$ENV{DESTDIR}" STREQUAL "")
        set(_share "${CMAKE_INSTALL_PREFIX}/share")
        execute_process(COMMAND update-mime-database "${_share}/mime" RESULT_VARIABLE _r ERROR_QUIET)
        execute_process(COMMAND update-desktop-database "${_share}/applications" ERROR_QUIET)
        execute_process(COMMAND gtk-update-icon-cache -f -t "${_share}/icons/hicolor" ERROR_QUIET)
        execute_process(COMMAND xdg-mime default phobicccp.desktop application/x-carbide-create ERROR_QUIET)
        message(STATUS "Desktop integration refreshed under ${_share}")
    endif()
]])
