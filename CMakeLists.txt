cmake_minimum_required(VERSION 3.19)
project(phobiCCCP VERSION 1.3.26 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(ANDROID)
    # Qt for Android has no zstd; the host rcc would compress with it.
    set(CMAKE_AUTORCC_OPTIONS "--no-zstd")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Network)
# The serial link to the machine. Qt has no serial ports on Android or iPad:
# there src/serialstub.h stands in and a pad sends its jobs to a computer.
find_package(Qt6 QUIET COMPONENTS SerialPort)
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
    src/cccpfile.cpp
    src/c2ddocument.cpp
    src/element.cpp
    src/gcodeexport.cpp
    src/cam3d.cpp
    src/vcarve.cpp
    src/post_grbl.cpp
    src/grblstreamer.cpp
    src/heightmodel.cpp
    src/surfacemap.cpp
    src/simulation.cpp
    src/vectorops.cpp
    src/sketch.cpp
    src/parametric.cpp
    src/adaptive.cpp
    src/sketchgeom.cpp
    src/setupsheet.cpp
    src/importers.cpp
    src/toollibrary.cpp
    src/toolcollections.cpp
    src/toolpathfactory.cpp
    src/tiling.cpp
    data/tools.qrc
    src/backgroundimage.cpp
    src/imagetrace.cpp
    src/model3d.cpp
    src/recovery.cpp
    src/machineprofiles.cpp
    src/padlink.cpp
)
# The binary should be able to say which build it is (--version, --help);
# PROJECT_VERSION is the single source of truth in this file. It reaches the
# code through a generated header (phobiversion.h) that only the files showing
# the version include, so a bump recompiles those and not everything.
configure_file(src/phobiversion.h.in "${CMAKE_BINARY_DIR}/generated/phobiversion.h" @ONLY)
target_include_directories(phobicccp_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(phobicccp_core PUBLIC
    PHOBICCCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}" PHOBICCCP_BUILD_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(phobicccp_core PUBLIC Qt6::Gui Qt6::Sql ZLIB::ZLIB)
if(Qt6SerialPort_FOUND)
    target_link_libraries(phobicccp_core PUBLIC Qt6::SerialPort)
    target_compile_definitions(phobicccp_core PUBLIC HAVE_SERIALPORT)
else()
    target_sources(phobicccp_core PRIVATE src/serialstub.h)
endif()

# The windows and panels, compiled once and shared by the app and every widget
# test. Tests used to list these sources themselves, so each test executable
# compiled its own copy (the main window tests the whole program, nine times).
add_library(phobicccp_ui STATIC
    src/canvas.cpp src/canvasparametric.cpp
    src/mainwindow.cpp
    src/aboutdialog.cpp
    src/propertiespanel.cpp
    src/parameterspanel.cpp
    src/toolpathpanel.cpp src/shapestrip.cpp
    src/dragscroll.cpp
    src/toolpathform.cpp
    src/toollibrarydialog.cpp
    src/tooldiagram.cpp
    src/toolstrip.cpp
    src/machinepanel.cpp
    src/jobview.cpp
    src/shopdialog.cpp
    src/isopreview.cpp
    src/simpanel.cpp
    src/vectoractions.cpp
    src/importui.cpp
    src/tracedialog.cpp
    src/backgrounddialog.cpp
    src/modelpanel.cpp
    src/exportprogress.cpp
    src/gamepad.cpp
    src/gamepadmapdialog.cpp
    src/bitzeropanel.cpp
    src/settingsdialog.cpp
    src/toolbarorder.cpp
    src/projectreceiver.cpp
    src/sendtopaddialog.cpp
    src/sendtodialog.cpp
)
target_link_libraries(phobicccp_ui PUBLIC phobicccp_core Qt6::Widgets Qt6::Network)

set(PHOBICCCP_APP_SOURCES src/main.cpp)
if(ANDROID)
    # The same program on a pad: Qt builds it as the app's library, and
    # qt-android/ holds the manifest (network access, landscape, the icon).
    qt_add_executable(phobicccp ${PHOBICCCP_APP_SOURCES})
    # Always rising, and above the Flutter pad app's build numbers.
    math(EXPR PHOBICCCP_VERSION_CODE
         "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_target_properties(phobicccp PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/qt-android
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE ${PHOBICCCP_VERSION_CODE}
        QT_ANDROID_TARGET_SDK_VERSION 34)
else()
    add_executable(phobicccp ${PHOBICCCP_APP_SOURCES})
endif()

target_link_libraries(phobicccp PRIVATE phobicccp_ui)
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

if(ANDROID)
    return()   # no tests on the pad build; CI runs them on Linux
endif()

# The developer build of the program: the same app plus the self-check,
# screenshot (--shot) and machine bring-up (--grbl-*) modes. The tests drive
# it; it is never installed or shipped. Test-only code goes here or in a
# test target, never into phobicccp or phobicccp_ui (the shipped_clean test
# below fails if any of it reaches the shipped program).
add_executable(phobicccp-dev src/main.cpp)
target_compile_definitions(phobicccp-dev PRIVATE PHOBICCCP_DEV_TOOLS)
target_link_libraries(phobicccp-dev PRIVATE phobicccp_ui)

# Tests: `ctest --test-dir build`. Geometry/CAM checks are plain asserts;
# the GRBL simulator and the machine flow test need python3.
enable_testing()
add_executable(test_robustness tests/test_robustness.cpp)
target_link_libraries(test_robustness PRIVATE phobicccp_core Qt6::Sql)
add_test(NAME robustness COMMAND test_robustness
         "$ENV{HOME}/Documents/c2d-samples/CarbideCreateAllPossibilities.c2d")

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration PRIVATE phobicccp_core)
add_test(NAME integration COMMAND test_integration)
set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_geometry tests/test_geometry.cpp)
target_link_libraries(test_geometry PRIVATE phobicccp_core)
add_test(NAME geometry COMMAND test_geometry)
add_executable(test_simulation tests/test_simulation.cpp)
target_link_libraries(test_simulation PRIVATE phobicccp_core)
add_test(NAME simulation COMMAND test_simulation)
add_executable(test_adaptive tests/test_adaptive.cpp)
target_link_libraries(test_adaptive PRIVATE phobicccp_core)
add_test(NAME adaptive COMMAND test_adaptive)
add_executable(test_sketch tests/test_sketch.cpp)
target_link_libraries(test_sketch PRIVATE phobicccp_core)
add_test(NAME sketch COMMAND test_sketch)
add_executable(test_parametric tests/test_parametric.cpp)
target_link_libraries(test_parametric PRIVATE phobicccp_core)
add_test(NAME parametric COMMAND test_parametric)
add_executable(test_vectorops tests/test_vectorops.cpp)
target_link_libraries(test_vectorops PRIVATE phobicccp_core)
add_test(NAME vectorops COMMAND test_vectorops)
# Projects sent from Syntexophobic: the protocol reader and a loopback transfer.
add_executable(test_receiver tests/test_receiver.cpp)
target_link_libraries(test_receiver PRIVATE phobicccp_ui)
add_test(NAME receiver COMMAND test_receiver)
# The phone app's side: pad designs into documents, the design store, machines.
add_executable(test_machinenames tests/test_machinenames.cpp)
target_link_libraries(test_machinenames PRIVATE phobicccp_core)
add_test(NAME machinenames COMMAND test_machinenames)
add_executable(test_padlink tests/test_padlink.cpp)
target_link_libraries(test_padlink PRIVATE phobicccp_core)
add_test(NAME padlink COMMAND test_padlink)
add_executable(test_newdoc tests/test_newdoc.cpp)
target_link_libraries(test_newdoc PRIVATE phobicccp_core)
add_test(NAME newdoc COMMAND test_newdoc)
add_executable(test_sketchgeom tests/test_sketchgeom.cpp)
target_link_libraries(test_sketchgeom PRIVATE phobicccp_core)
add_test(NAME sketchgeom COMMAND test_sketchgeom)
add_executable(test_import tests/test_import.cpp)
target_link_libraries(test_import PRIVATE phobicccp_core)
target_compile_definitions(test_import PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME import COMMAND test_import)
add_executable(test_cccpfile tests/test_cccpfile.cpp)
target_link_libraries(test_cccpfile PRIVATE phobicccp_core)
target_compile_definitions(test_cccpfile PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME cccpfile COMMAND test_cccpfile)
# The tool-change rehearsal program: what it may and may not send.
add_executable(test_rehearsal tests/test_rehearsal.cpp)
target_link_libraries(test_rehearsal PRIVATE phobicccp_core)
add_test(NAME rehearsal COMMAND test_rehearsal)
# Surface map: interpolation, storage, probe macros, applying it to a program.
add_executable(test_surfacemap tests/test_surfacemap.cpp)
target_link_libraries(test_surfacemap PRIVATE phobicccp_core)
add_test(NAME surfacemap COMMAND test_surfacemap)
# The streamer against a scripted controller on a pty (Linux; skips when no
# pty opens as a serial port).
if(UNIX AND NOT APPLE)
    add_executable(test_streamer tests/test_streamer.cpp)
    target_link_libraries(test_streamer PRIVATE phobicccp_core util)
    add_test(NAME streamer COMMAND test_streamer)
    set_tests_properties(streamer PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_executable(test_tiling tests/test_tiling.cpp)
target_link_libraries(test_tiling PRIVATE phobicccp_core)
add_test(NAME tiling COMMAND test_tiling)
add_executable(test_trace tests/test_trace.cpp)
target_link_libraries(test_trace PRIVATE phobicccp_core)
add_test(NAME trace COMMAND test_trace)
add_executable(test_paths tests/test_paths.cpp)
target_link_libraries(test_paths PRIVATE phobicccp_core)
add_test(NAME paths COMMAND test_paths)
add_executable(test_model3d tests/test_model3d.cpp)
target_link_libraries(test_model3d PRIVATE phobicccp_core)
add_test(NAME model3d COMMAND test_model3d)
add_executable(test_cam3d tests/test_cam3d.cpp)
target_link_libraries(test_cam3d PRIVATE phobicccp_core)
add_test(NAME cam3d COMMAND test_cam3d)
# Regressions from the hardening pass: medial-axis spurs, the tile boundary,
# contour tabs, string cut parameters, hostile DXF, atomic save, model rows
# that will not decode. Takes the sample document for the save section.
add_executable(test_hardening tests/test_hardening.cpp)
target_link_libraries(test_hardening PRIVATE phobicccp_core Qt6::Sql)
add_test(NAME hardening COMMAND test_hardening
         "$ENV{HOME}/Documents/c2d-samples/CarbideCreateAllPossibilities.c2d")
# Canvas interaction test (pen tool, node editor) drives the real widget with
# synthetic events; needs the optional Qt6 Test module.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    add_executable(test_canvas tests/test_canvas.cpp)
    target_link_libraries(test_canvas PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME canvas COMMAND test_canvas)
    # Area tool on hair-thin islands, and pattern copies round a closed path.
    add_executable(test_areathin tests/test_areathin.cpp)
    target_link_libraries(test_areathin PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME areathin COMMAND test_areathin)
    add_executable(test_patternclosed tests/test_patternclosed.cpp)
    target_link_libraries(test_patternclosed PRIVATE phobicccp_core)
    add_test(NAME patternclosed COMMAND test_patternclosed)
    add_executable(test_ringisland tests/test_ringisland.cpp)
    target_link_libraries(test_ringisland PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME ringisland COMMAND test_ringisland)
    add_executable(test_escdrag tests/test_escdrag.cpp)
    target_link_libraries(test_escdrag PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME escdrag COMMAND test_escdrag)
    add_executable(test_sendto tests/test_sendto.cpp)
    target_link_libraries(test_sendto PRIVATE phobicccp_ui)
    add_test(NAME sendto COMMAND test_sendto)
    add_executable(test_sendtopad tests/test_sendtopad.cpp)
    target_link_libraries(test_sendtopad PRIVATE phobicccp_ui)
    add_test(NAME sendtopad COMMAND test_sendtopad)
    add_executable(test_canvasparametric tests/test_canvasparametric.cpp)
    target_link_libraries(test_canvasparametric PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME canvasparametric COMMAND test_canvasparametric)
    add_executable(test_about tests/test_about.cpp data/tools.qrc)
    target_link_libraries(test_about PRIVATE phobicccp_ui)
    add_test(NAME about COMMAND test_about)
    # Tool library popup: the bit-kind icons that sort the list.
    add_executable(test_toollibrarydialog tests/test_toollibrarydialog.cpp data/tools.qrc)
    target_link_libraries(test_toollibrarydialog PRIVATE phobicccp_ui)
    add_test(NAME toollibrarydialog COMMAND test_toollibrarydialog)
    add_executable(test_toolpathform tests/test_toolpathform.cpp)
    target_link_libraries(test_toolpathform PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME toolpathform COMMAND test_toolpathform)
    # Panel <-> canvas selection handover (needs the real widgets).
    add_executable(test_panels tests/test_panels.cpp)
    target_link_libraries(test_panels PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME panels COMMAND test_panels)
    set_tests_properties(panels PROPERTIES SKIP_RETURN_CODE 77)
    # Two bits on one tool number (setup sheet, Simulate strip) and inlay bindings.
    add_executable(test_uifixes tests/test_uifixes.cpp)
    target_link_libraries(test_uifixes PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME uifixes COMMAND test_uifixes)
    # The main window's Preview (export on a worker, stale results dropped)
    # and its close guard: the whole window, so all of phobicccp_ui.
    add_executable(test_mainwindow tests/test_mainwindow.cpp)
    target_link_libraries(test_mainwindow PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME mainwindow COMMAND test_mainwindow)
    # A pad's edit arriving while a typed number is not yet committed.
    add_executable(test_padput tests/test_padput.cpp)
    target_link_libraries(test_padput PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME padput COMMAND test_padput)
    add_executable(test_padenter tests/test_padenter.cpp)
    target_link_libraries(test_padenter PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME padenter COMMAND test_padenter)
    add_executable(test_tpflush tests/test_tpflush.cpp)
    target_link_libraries(test_tpflush PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME tpflush COMMAND test_tpflush)
    add_executable(test_padmachinemodal tests/test_padmachinemodal.cpp)
    target_link_libraries(test_padmachinemodal PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME padmachinemodal COMMAND test_padmachinemodal)
    add_executable(test_typedclick tests/test_typedclick.cpp)
    target_link_libraries(test_typedclick PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME typedclick COMMAND test_typedclick)
    # Delete with a value still typed in the toolpath form.
    add_executable(test_tpdelete tests/test_tpdelete.cpp)
    target_link_libraries(test_tpdelete PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME tpdelete COMMAND test_tpdelete)
    # The "v" under a toolpath card: thumbnails, their bins, what is left to cut.
    add_executable(test_tpshapes tests/test_tpshapes.cpp)
    target_link_libraries(test_tpshapes PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME tpshapes COMMAND test_tpshapes)
    # STL import's default size for a mesh too big for the board.
    add_executable(test_stlplace tests/test_stlplace.cpp)
    target_link_libraries(test_stlplace PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME stlplace COMMAND test_stlplace)
    # Crash recovery offered at start-up waits for the command-line file's open.
    add_executable(test_recoverystart tests/test_recoverystart.cpp)
    target_link_libraries(test_recoverystart PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME recoverystart COMMAND test_recoverystart)
    # A pad's edit already queued when Save commits a typed number.
    add_executable(test_padputsave tests/test_padputsave.cpp)
    target_link_libraries(test_padputsave PRIVATE phobicccp_ui Qt6::Test)
    foreach(mode save close nosave)
        add_test(NAME padputsave_${mode} COMMAND test_padputsave ${mode})
    endforeach()
    # Drag-scrolling on the Toolpaths side, and cards that scroll instead of moving.
    add_executable(test_dragscroll tests/test_dragscroll.cpp)
    target_link_libraries(test_dragscroll PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME dragscroll COMMAND test_dragscroll)
    # Mirror / arrays through the real Edit → Vectors actions and undo stack.
    add_executable(test_vectoractions tests/test_vectoractions.cpp)
    target_link_libraries(test_vectoractions PRIVATE phobicccp_ui)
    add_test(NAME vectoractions COMMAND test_vectoractions)
else()
    message(STATUS "Qt6 Test not found: canvas interaction test skipped")
endif()
# The threaded export helper lives in the app target, so the test compiles it
# in directly. Worth re-running under ThreadSanitizer after any change to it.
add_executable(test_exportjob tests/test_exportjob.cpp)
target_link_libraries(test_exportjob PRIVATE phobicccp_ui)
add_test(NAME exportjob COMMAND test_exportjob)

# The gamepad reader, driven through a fifo carrying synthetic joystick events
# so it can be tested with no controller plugged in.
if(UNIX AND NOT APPLE AND Qt6Test_FOUND)
    add_executable(test_gamepad tests/test_gamepad.cpp)
    target_link_libraries(test_gamepad PRIVATE phobicccp_ui Qt6::Test)
    add_test(NAME gamepad COMMAND test_gamepad)
    # The Machine panel on a fake controller: tool-change prompt, zeroing,
    # jogging, cancelling and disconnecting around a job.
    add_executable(test_machine tests/test_machine.cpp)
    target_link_libraries(test_machine PRIVATE phobicccp_ui)
    add_test(NAME machine COMMAND test_machine)
    # Over two minutes on its own: start it first so ctest -j runs the rest
    # beside it instead of after it.
    set_tests_properties(machine PROPERTIES SKIP_RETURN_CODE 77 COST 1000)
    # Cancel job / Disconnect while the hold settles: a Stop in the wait, and
    # the job's last lines answered in it.
    # A machine switch reloads the BitZero page's plate settings.
    add_executable(test_bzprofile tests/test_bzprofile.cpp)
    target_link_libraries(test_bzprofile PRIVATE phobicccp_ui)
    add_test(NAME bzprofile COMMAND test_bzprofile)
    add_executable(test_machinestop tests/test_machinestop.cpp)
    target_link_libraries(test_machinestop PRIVATE phobicccp_ui)
    foreach(mode cancelstop cancelend disconnend bzstale cancelresume safez descend tlo descendlive tc holdend parkend gotoend measureend rehearsetc zeroend zerooff zerounhomed bzoff deaf bztap rehdeaf endtap steptap stephold bzstart jogstart jogsettle jogsettlerun rerun rehtap parktap bzjog measuretap gototap homepark homepark0 homejog tloretry homerej tlospam remoteset ovrreset v1miss v1reboot maphigh connhold envxy envz inch13 inch13wco connrun mapzero mapzeropad mapremeasure mapzeroreconn maptlo tlo2 jobstop jobcancel jobreset jobtcstop jobcount jobclear)
        add_test(NAME machinestop_${mode} COMMAND test_machinestop)
        set_tests_properties(machinestop_${mode} PROPERTIES SKIP_RETURN_CODE 77
                             ENVIRONMENT "R4MODE=${mode};QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME grblsim COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/grblsim_selftest.py)
    add_test(NAME machine_flow COMMAND ${CMAKE_SOURCE_DIR}/tools/flowtest.sh $<TARGET_FILE:phobicccp-dev>)
    # The failure paths: an over-long line, a line GRBL rejects mid-program,
    # and a critical alarm. Each of these used to hang the sender for ever.
    add_test(NAME machine_faults COMMAND ${CMAKE_SOURCE_DIR}/tools/streamtest.sh $<TARGET_FILE:phobicccp-dev>)
    # Both drive a real pty against wall-clock deadlines (3 s for the banner,
    # 30 s for a homing cycle). Sharing the box with ctest -j starves them and
    # they fail on timing alone, so they get the machine to themselves.
    set_tests_properties(grblsim machine_flow machine_faults PROPERTIES RUN_SERIAL TRUE)
endif()

# Nothing test-only in the program people get: no developer modes or
# screenshot hooks in the shipped binary, and no test sources linked into it.
add_test(NAME shipped_clean COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE:phobicccp>
         "-DSOURCES=$<JOIN:$<TARGET_PROPERTY:phobicccp,SOURCES>;$<TARGET_PROPERTY:phobicccp_ui,SOURCES>,|>"
         -P ${CMAKE_SOURCE_DIR}/tests/shipped_clean.cmake)

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
