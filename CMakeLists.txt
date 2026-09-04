cmake_minimum_required(VERSION 3.19)
project(phobiCCCP VERSION 0.1.6 LANGUAGES CXX)

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

add_executable(phobicccp
    src/main.cpp
    src/zlibutil.cpp
    src/c2ddocument.cpp
    src/element.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/propertiespanel.cpp
    src/toolpathpanel.cpp
    src/gcodeexport.cpp
    src/vcarve.cpp
    src/post_grbl.cpp
    src/grblstreamer.cpp
    src/machinepanel.cpp
)

target_link_libraries(phobicccp PRIVATE Qt6::Widgets Qt6::Sql Qt6::SerialPort ZLIB::ZLIB)
if(PHOBICCCP_WITH_CLIPPER2)
    target_link_libraries(phobicccp PRIVATE Clipper2)
    target_compile_definitions(phobicccp PRIVATE HAVE_CLIPPER2)
endif()

# True medial-axis v-carve needs the (header-only) Boost.Polygon Voronoi
# builder; without it the exporter falls back to depth-graded inset rings.
find_path(BOOST_VORONOI_INC boost/polygon/voronoi.hpp)
if(BOOST_VORONOI_INC)
    target_include_directories(phobicccp PRIVATE ${BOOST_VORONOI_INC})
    target_compile_definitions(phobicccp PRIVATE HAVE_BOOST_VORONOI)
    message(STATUS "Boost.Polygon Voronoi found: medial-axis v-carve enabled")
else()
    message(STATUS "Boost.Polygon Voronoi NOT found: v-carve uses ring fallback")
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
