cmake_minimum_required(VERSION 3.16)
# THE STUDIO'S VERSION — the one place it is set. It is baked into StudioVersion.h (below), shown in
# Help > About, and compared against the newest GitHub release tag by the update check. A release
# tagged vX.Y.Z must be built with X.Y.Z here.
project(jframework_studio VERSION 0.3.4 LANGUAGES CXX)


# THE TEST SUITE IS A SUITE. Thirty add_test() registrations sat here doing nothing because
# enable_testing() was never called: ctest reported "No tests were found", so every check had to be run
# by executing its binary by hand — which is how several came to be failing, and three to have stopped
# compiling, without anyone noticing. The targets stay EXCLUDE_FROM_ALL (fifty binaries is a slow
# default build), so `studio_tests` builds them and ctest then runs them:
#
#     cmake --build build --target studio_tests && ctest --test-dir build
enable_testing()
add_custom_target(studio_tests COMMENT "build every studio test binary")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BUILD SPEED, MEASURED. A one-line change to main.cpp cost 134 s to a usable binary: ~50 s compiling
# that one 5,225-line translation unit and the rest linking 239 MB of debug info. A change to a
# JFramework header cost four minutes, because the installed SDK headers are not dependency-tracked, so
# every one of the 147 translation units rebuilds whether its own inputs moved or not.
#
#   ccache — answers that second case. The TUs whose preprocessed input really is unchanged come back
#           from cache instead of being compiled again. It is a launcher, so it applies to every target
#           here including the tests, and it is skipped silently when not installed.
#   mold   — answers the link. A 239 MB binary is mostly debug info to relocate, which is exactly what
#           mold parallelises; GNU ld does it on one thread.
#
# Neither changes what is built: same compiler, same flags, same output.
find_program(STUDIO_CCACHE ccache)
if(STUDIO_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${STUDIO_CCACHE})
    message(STATUS "studio: compiling through ccache (${STUDIO_CCACHE})")
endif()
# AND main.cpp IS COMPILED AT -O1 — NOT -O0, AND THE REASON IS NOT ABOUT main.cpp.
#
# Its compile IS the cost of every UI change: 133 s at -O2 against a link (with mold) of under a second,
# so it was tried at -O0, which took it to 50 s. The studio then showed its window in 0.1 s and its
# interface THREE SECONDS later, and moving the heavy work out of this unit did not fix it.
#
# Because the work was never really in it. This framework is header-only: JJson::parseFile, PanelModel::
# load and most of JWidget are inline, so every unit that uses them emits its own weak copy and the LINKER
# KEEPS ONE. Build a single unit at -O0 and its unoptimised copies can be the ones the whole program ends
# up calling — measured, from inside a -O2 unit: the same PanelLibrary::parseDocument reported 62 ms with
# main.cpp at -O1 and 1,065 ms with main.cpp at -O0. One file turned the parser down for everybody.
#
# So the floor here is not a matter of taste, and splitting this file would not raise it. -O1 keeps a
# quarter off the build (100 s vs 133 s) with a startup indistinguishable from -O2: window 0.1 s,
# interface 0.9 s, whole project open 229 ms.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-O1")
# …and that -O1 is why main.cpp cannot share the target's precompiled header. A PCH is only usable by a
# TU compiled with the same flags, and -O1 defines __NO_INLINE__ differently from the no-optimisation
# default the PCH is built at — so GCC rejected it on every build with
#   "cmake_pch.hxx.gch: not used because '__NO_INLINE__' not defined".
# The header was not being used here either way; saying so is the difference between a build that is
# clean and a build with one warning everybody has learned to scroll past.
set_source_files_properties(main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

find_program(STUDIO_MOLD mold)
if(STUDIO_MOLD)
    add_link_options(-fuse-ld=mold)
    message(STATUS "studio: linking with mold (${STUDIO_MOLD})")
endif()

# The studio, on JFramework. Consumes the framework as an
# installed SDK — nothing from the studio leaks back into the framework.
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/jframework-sdk
find_package(JFramework 0.1 CONFIG REQUIRED)

# App source: the shell (main.cpp) + the ECU model/data backend (src/model/*).
# The model is app code — it consumes the framework's
# primitives (JJson, JSignal, JUndoStack, JSettings), never the reverse.
file(GLOB STUDIO_MODEL_SRC   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/model/*.cpp)
file(GLOB STUDIO_COMMS_SRC   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/comms/*.cpp)
file(GLOB STUDIO_SURFACE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/surface/*.cpp)
file(GLOB STUDIO_WIDGET_SRC  CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/surface/widgets/*.cpp)
# App shell pieces split out of main.cpp — every one of these is a separate TU, which is the point: main.cpp
# was a single object big enough to need -Wa,-mbig-obj below.
file(GLOB STUDIO_APP_SRC     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)
file(GLOB STUDIO_UI_SRC      CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp)

# COMPILED ONCE, LINKED EVERYWHERE.
#
# Every test binary used to LIST these sources, so the model and widget layers were compiled again for
# each one: 93 targets, 160 unique files, 3465 compilations — a 21.7x duplication factor. A header
# change meant rebuilding the same eighty files eighty times, which is most of why a full test build
# took half an hour and pinned every core on the machine.
#
# OBJECT libraries, not static ones: a widget registers itself from a file-scope initialiser
# (REGISTER_WIDGET), and a static archive drops an object nothing references — so the widgets would
# vanish from any binary that did not name them. Object libraries hand every object to the linker.
# PRECOMPILED HEADERS. The framework is header-only and its core header pulls 372 files behind it —
# <format>, <locale>, <iostream>, <ranges>, <sstream>, <chrono> among them — so every translation unit
# spent ~2 seconds on the same soup before compiling a line of its own code. 68 files in the app, ~90
# more across the model and surface: that was most of the build.
#
# Only the app's own targets share it. The test binaries each carry their own -D (a fixture path), and
# GCC rejects a precompiled header built with different macros — so they keep compiling the headers
# themselves, which is now one TU each rather than sixty.
# …and the STUDIO's own heavy headers, not just the framework's. CanvasWidget.h alone preprocesses to
# 149,000 lines and is included by main.cpp and all forty widget units; Cache.h and MetaModel.h are in
# nearly as many. Precompiling what every unit already includes is the cheapest compile time there is.
set(STUDIO_PCH
    <j/core/JWidget.h>
    <j/core/JStyle.h>
    <j/config/Json.h>
    ${CMAKE_CURRENT_SOURCE_DIR}/src/surface/CanvasWidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/Cache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/MetaModel.h
    <string> <vector> <memory> <map> <unordered_map> <algorithm> <cmath> <cstdint> <functional>)

add_library(studio_model OBJECT ${STUDIO_MODEL_SRC} ${STUDIO_COMMS_SRC})
target_include_directories(studio_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(studio_model PUBLIC JFramework::platform)
target_precompile_headers(studio_model PRIVATE ${STUDIO_PCH})

# Resources (where the executable is, what ships beside it) is asked by the surface too — the shipped
# calibration presets (Surface::shippedCalibrationDir) — so it builds into the surface library, which
# the test binaries link, rather than only into the executable.
list(REMOVE_ITEM STUDIO_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Resources.cpp)
add_library(studio_surface OBJECT ${STUDIO_SURFACE_SRC} ${STUDIO_WIDGET_SRC}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Resources.cpp)
target_include_directories(studio_surface PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(studio_surface PUBLIC JFramework::platform)
target_precompile_headers(studio_surface PRIVATE ${STUDIO_PCH})

# THE OBJECT LIBRARIES ARE ALREADY BUILT — LINK THEM, DO NOT COMPILE THEM AGAIN. This listed every model,
# comms, surface and widget source a SECOND time, so a full build compiled the whole codebase twice: once
# into studio_model/studio_surface (which the test binaries link) and once more into the executable. Ninety
# files, every time, for nothing — and editing one widget compiled it twice before the link.
add_executable(studio main.cpp ${STUDIO_APP_SRC} ${STUDIO_UI_SRC})
target_link_libraries(studio PRIVATE studio_model studio_surface)
target_include_directories(studio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/app/StudioVersion.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/StudioVersion.h @ONLY)
# The udev rule the studio installs for itself on Linux (FirmwareUpgrade.cpp, through JUdevRule): read from the ONE copy in
# the firmware tree at configure time, so the rule the studio installs is always the rule in the repo.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/udev/70-jayecu.rules)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/udev/70-jayecu.rules JAYECU_UDEV_RULES)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/app/UdevRules.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/UdevRules.h @ONLY)
target_include_directories(studio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# EVERYTHING THE STUDIO SHIPS WITH TRAVELS WITH THE BINARY, because that is the one place it can be
# found from: resources::path looks beside the executable first, so a studio started from a shortcut,
# by full path, or from any other directory still comes up with its artwork and its own theme.
# studio.style is the app's Dark palette and studio.common.style its proportions — BOTH are needed,
# in either theme. Without them the studio silently wears the framework's default sizes, which is how
# it looked when launched from anywhere but the source tree, and how LIGHT looked everywhere.
foreach(res
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/jaytek-logo.png
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/jaytek-landing.png
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/UbuntuSans.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/UbuntuSans-LICENCE.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/studio.common.style
        ${CMAKE_CURRENT_SOURCE_DIR}/studio.style)
    add_custom_command(TARGET studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${res} $<TARGET_FILE_DIR:studio>)
endforeach()
target_precompile_headers(studio PRIVATE ${STUDIO_PCH})
# JHttpClient (in the SDK's libj_platform.a) uses OpenSSL for TLS, and a static lib exports no
# dependencies of its own — so the application links them.  On Windows TLS is WinHTTP's: the library
# carries no OpenSSL symbols at all, so there is nothing to find and nothing to link.
if(WIN32)
    set(STUDIO_TLS_LIBS "")
else()
    find_package(OpenSSL REQUIRED)
    set(STUDIO_TLS_LIBS ${STUDIO_TLS_LIBS})
endif()
target_link_libraries(studio PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

if(WIN32)
    # The application icon. Windows takes an executable's icon from its RESOURCES, not from a file
    # beside it, so the .ico is compiled in rather than shipped alongside like the PNGs.
    enable_language(RC)
    target_sources(studio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/studio.rc)
    # main.cpp is a single huge TU → the default COFF object overflows its section limit ("file too big").
    target_compile_options(studio PRIVATE -Wa,-mbig-obj)
    # Standalone .exe: fold the MinGW C/C++ runtimes in so no extra DLLs are needed in the VM.
    target_link_options(studio PRIVATE -static -static-libgcc -static-libstdc++)
    # FIRMWARE OVER USB (src/comms/DfuUsbWindows.cpp): SetupAPI finds the bootloader, WinUSB talks to it,
    # ole32 reads its interface GUID. And the driver installer beside the exe, in driver/, which binds
    # WinUSB to the bootloader through the administrator prompt (third_party/libwdi-win).
    target_link_libraries(studio PRIVATE setupapi winusb ole32)
    # A WINDOWS program, not a console one: built as a console application, Windows opened a terminal
    # window beside the studio every time it started. Its output goes to genesis.log, not a console.
    # MinGW's startup code still calls main() for a GUI-subsystem program, so nothing else changes.
    set_target_properties(studio PROPERTIES WIN32_EXECUTABLE TRUE)
    add_custom_command(TARGET studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:studio>/driver
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwdi-win/wdi-simple.exe $<TARGET_FILE_DIR:studio>/driver/)
endif()

# Headless regression test for the trigger designer's logic core — no UI, but TriggerWheel reads and
# writes the wheel library as JSON, so it needs the framework's Json.
# The config the STUDIO would write for a named wheel, taken through the designer — so the bench can
# apply exactly those bytes and ask a real ECU whether it decodes them.
#   cmake --build build --target wheel_emit && ./build/wheel_emit <meta> "<wheel>"
add_executable(wheel_emit EXCLUDE_FROM_ALL
    tests/wheel_emit.cpp)   # TriggerWheel now applies a wheel THROUGH the Cache (unit conversion), so the
                           # whole model layer comes with it rather than two hand-picked files
target_include_directories(wheel_emit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                              ${CMAKE_CURRENT_SOURCE_DIR}/src/model
                                              ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                              ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(wheel_emit PRIVATE studio_model)
target_link_libraries(wheel_emit PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# What the OUTPUT WIZARD would write, for every template, as JSON — so a bench can put it on a real
# ECU and drive it. templateWrites() is the function the dialog's Apply calls, so this is that path
# and not a second copy of it; what it leaves out is the dialog.
#   cmake --build build --target template_emit && ./build/template_emit <meta> [slot]
#   python3 tools/bench_output_templates.py --writes <that json>
add_executable(template_emit EXCLUDE_FROM_ALL
    tests/template_emit.cpp)
target_include_directories(template_emit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(template_emit PRIVATE studio_model)
target_link_libraries(template_emit PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# What a .tune brings across a firmware update, without the studio:
# The data behind "Apply to Base Table" — that the schema's apply_to survives codegen + MetaModel, and
# that the trim really does share its base table's axes (which is what makes the roll-in a copy).
#   cmake --build build --target apply_to_base && ./build/apply_to_base <meta>
add_executable(apply_to_base EXCLUDE_FROM_ALL
    tests/apply_to_base.cpp)
target_include_directories(apply_to_base PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(apply_to_base PRIVATE studio_model)
target_link_libraries(apply_to_base PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

#   cmake --build build --target tune_migrate && ./build/tune_migrate <meta> <tune> [path ...]
add_executable(tune_migrate EXCLUDE_FROM_ALL
    tests/tune_migrate.cpp)
target_include_directories(tune_migrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                                ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(tune_migrate PRIVATE studio_model)
target_link_libraries(tune_migrate PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# On demand:  cmake --build build --target trigger_core_test && ./build/trigger_core_test
# The whole model layer, not just the two trigger TUs: applyWheelToConfig writes through the Cache, so
# the wheel no longer stands alone.
add_executable(trigger_core_test EXCLUDE_FROM_ALL
    tests/test_trigger_core.cpp)
target_link_libraries(trigger_core_test PRIVATE studio_model)
target_include_directories(trigger_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/src/model)
target_link_libraries(trigger_core_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(trigger_core_test PRIVATE
    META_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# Headless regression for the Cache block abstraction (config vs the battery-backed learned segment).
#   cmake --build build --target cache_blocks_test && ./build/cache_blocks_test
# Tune-CRC watch — the rules deciding when an ECU-side config change forces a re-read.
add_executable(tune_crc_watch_test EXCLUDE_FROM_ALL tests/test_tune_crc_watch.cpp)
target_link_libraries(tune_crc_watch_test PRIVATE studio_model)
target_include_directories(tune_crc_watch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tune_crc_watch_test PRIVATE JFramework::platform)

# resolveStrip — a run of values addressed as one thing (a 1-D array, or a nested sub-array inside one
# element). Every way of getting its arithmetic wrong is silent, hence a test rather than a look.
#   cmake --build build --target runs_test && ./build/runs_test
add_executable(runs_test EXCLUDE_FROM_ALL tests/test_runs.cpp)
target_link_libraries(runs_test PRIVATE studio_model)
target_include_directories(runs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runs_test PRIVATE JFramework::platform)
target_compile_definitions(runs_test PRIVATE
    FIXTURE_META="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test-meta.json")

add_executable(cache_blocks_test EXCLUDE_FROM_ALL tests/test_cache_blocks.cpp)
target_link_libraries(cache_blocks_test PRIVATE studio_model)
target_include_directories(cache_blocks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cache_blocks_test PRIVATE JFramework::platform)
target_compile_definitions(cache_blocks_test PRIVATE
    FIXTURE_META="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test-meta.json")

# Expression compiler (studio) x VM (firmware): every program is compiled here and then RUN by the
# firmware's own expr::exec, so the two halves are tested against EACH OTHER, not each against
# itself. Needs the firmware tree for Expr.h/SignalBus.h and generated/ for the signal catalog.
#   cmake --build build --target expr_compiler_test && ./build/expr_compiler_test
add_executable(expr_compiler_test EXCLUDE_FROM_ALL
    tests/test_expr_compiler.cpp
    )
target_link_libraries(expr_compiler_test PRIVATE studio_model)
target_include_directories(expr_compiler_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(expr_compiler_test PRIVATE JFramework::platform)
# THE META, BY ABSOLUTE PATH. The test hunted for it up two relative paths and SKIPPED (returning 0)
# when neither hit — which is what it did under ctest, so "expr_compiler PASSED" meant the test had
# not run since the day the working directory changed.
target_compile_definitions(expr_compiler_test PRIVATE
    META_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# Expression storage in the tune: source in, recompiled on load, reported when it cannot compile.
#   cmake --build build --target expr_tune_test && ./build/expr_tune_test
add_executable(expr_tune_test EXCLUDE_FROM_ALL
    tests/test_expr_tune.cpp
    )
target_link_libraries(expr_tune_test PRIVATE studio_model)
target_include_directories(expr_tune_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(expr_tune_test PRIVATE JFramework::platform)

# The UI path end to end: the real ExpressionWidget writes into a real config image, and the
# FIRMWARE's own VM runs the resulting bytes.
#   cmake --build build --target expr_widget_test && ./build/expr_widget_test
add_executable(expr_widget_test EXCLUDE_FROM_ALL
    tests/test_expr_widget.cpp
    )
target_link_libraries(expr_widget_test PRIVATE studio_surface studio_model)
target_include_directories(expr_widget_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(expr_widget_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(expr_widget_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# The wiring row's trip points: shown for a switch on an analog pin and for nothing else, reading the
# same two cal breakpoints the firmware decodes against.
#   cmake --build build --target wiring_thresh_test && ./build/wiring_thresh_test
# A digital output slot reads as a state, not a percentage — at the firmware's own threshold.
#   cmake --build build --target output_readout_test && ./build/output_readout_test
# The studio recorder writes an MSL MegaLogViewer can read: header shape, and every row the same width.
#   cmake --build build --target datalog_recorder_test && ./build/datalog_recorder_test
add_executable(datalog_recorder_test EXCLUDE_FROM_ALL
    tests/test_datalog_recorder.cpp
    )
target_link_libraries(datalog_recorder_test PRIVATE studio_model)
target_include_directories(datalog_recorder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(datalog_recorder_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(datalog_recorder_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME datalog_recorder COMMAND datalog_recorder_test)
add_dependencies(studio_tests datalog_recorder_test)

# Host variables: the definition's (meta "pcVars") and the project's (side-car) kept apart.
#   cmake --build build --target pc_vars_test && ./build/pc_vars_test
add_executable(pc_vars_test EXCLUDE_FROM_ALL
    tests/test_pc_vars.cpp
    )
target_link_libraries(pc_vars_test PRIVATE studio_model)
target_include_directories(pc_vars_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(pc_vars_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(pc_vars_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME pc_vars COMMAND pc_vars_test)
add_dependencies(studio_tests pc_vars_test)

# Collecting the ECU's logs off the card — a real directory of real MLG bytes, because the card at
# key-off is exactly that and nothing about this path is worth mocking.
#   cmake --build build --target card_logs_test && ./build/card_logs_test
add_executable(card_logs_test EXCLUDE_FROM_ALL
    tests/test_card_logs.cpp
    )
target_link_libraries(card_logs_test PRIVATE studio_model)
target_include_directories(card_logs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(card_logs_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME card_logs COMMAND card_logs_test)

add_executable(output_readout_test EXCLUDE_FROM_ALL
    tests/test_output_readout.cpp
    )
target_link_libraries(output_readout_test PRIVATE studio_surface studio_model)
target_include_directories(output_readout_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(output_readout_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(output_readout_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

add_executable(wiring_thresh_test EXCLUDE_FROM_ALL
    tests/test_wiring_thresh.cpp
    )
target_link_libraries(wiring_thresh_test PRIVATE studio_surface studio_model)
target_include_directories(wiring_thresh_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(wiring_thresh_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(wiring_thresh_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# The multi-position switch's band editor: shown in place of the curve, Capture keeps bands clear of their
# neighbours and the 0 V floor, and what it stores is a calibration the FIRMWARE's own rules accept.
#   cmake --build build --target bands_widget_test && ./build/bands_widget_test
# The Receive page's signal row: the ✕ that unsets a field's channel, pressed where it is drawn.
#   cmake --build build --target gc_signal_clear_test && ./build/gc_signal_clear_test
add_executable(gc_signal_clear_test EXCLUDE_FROM_ALL
    tests/test_gc_signal_clear.cpp
    )
target_link_libraries(gc_signal_clear_test PRIVATE studio_surface studio_model)
target_include_directories(gc_signal_clear_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(gc_signal_clear_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(gc_signal_clear_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_dependencies(studio_tests gc_signal_clear_test)
add_test(NAME gc_signal_clear COMMAND gc_signal_clear_test)

# The CAN field picker: the list it builds out of the tune's frames, and that a stored choice survives
# the pool being repacked and reads as broken when its field is gone.
#   cmake --build build --target can_field_picker_test && ./build/can_field_picker_test
add_executable(can_field_picker_test EXCLUDE_FROM_ALL
    tests/test_can_field_picker.cpp
    )
target_link_libraries(can_field_picker_test PRIVATE studio_surface studio_model)
target_include_directories(can_field_picker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(can_field_picker_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(can_field_picker_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_dependencies(studio_tests can_field_picker_test)
add_test(NAME can_field_picker COMMAND can_field_picker_test)

add_executable(bands_widget_test EXCLUDE_FROM_ALL
    tests/test_bands_widget.cpp
    tests/firmware_bands.cpp
    )
target_link_libraries(bands_widget_test PRIVATE studio_surface studio_model)
target_include_directories(bands_widget_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated/modules)
target_link_libraries(bands_widget_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(bands_widget_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME bands_widget COMMAND bands_widget_test)
add_dependencies(studio_tests bands_widget_test)

# ONE grammar, two back-ends: the shared parser (ExprAst) feeds both MathEvaluator (host) and
# ExprCompiler (firmware), and the two must agree on every expression both can run.
#   cmake --build build --target expr_unified_test && ./build/expr_unified_test
add_executable(expr_unified_test EXCLUDE_FROM_ALL
    tests/test_expr_unified.cpp
    )
target_link_libraries(expr_unified_test PRIVATE studio_model)
target_include_directories(expr_unified_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(expr_unified_test PRIVATE JFramework::platform)

# enableCondition: does a disabled control actually refuse input, or is it only greyed?
#   cmake --build build --target enable_condition_test && ./build/enable_condition_test
add_executable(enable_condition_test EXCLUDE_FROM_ALL
    tests/test_enable_condition.cpp
    )
target_link_libraries(enable_condition_test PRIVATE studio_surface studio_model)
target_include_directories(enable_condition_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(enable_condition_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME enable_condition COMMAND enable_condition_test)
add_dependencies(studio_tests enable_condition_test)

# [%connected]: does the link-live sigil track the flag, is it listed in the builder, does the
# editable-offline-or-stopped condition hold?
#   cmake --build build --target connected_sigil_test && ./build/connected_sigil_test
add_executable(connected_sigil_test EXCLUDE_FROM_ALL
    tests/test_connected_sigil.cpp src/ui/DictionaryTree.cpp
    )
target_link_libraries(connected_sigil_test PRIVATE studio_surface studio_model)
target_include_directories(connected_sigil_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(connected_sigil_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME connected_sigil COMMAND connected_sigil_test)
add_dependencies(studio_tests connected_sigil_test)

# The toolbar Burn button's three states (clean / dirty / just-burned) and the transitions between them.
#   cmake --build build --target burn_button_test && ./build/burn_button_test
add_executable(burn_button_test EXCLUDE_FROM_ALL tests/test_burn_button.cpp)
target_include_directories(burn_button_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(burn_button_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME burn_button COMMAND burn_button_test)
add_dependencies(studio_tests burn_button_test)

# A press inside a viewport captures the pointer: drag off a button and it springs back, release off it
# cancels the click, and neither leaves it stuck looking pressed.
#   cmake --build build --target viewport_capture_test && ./build/viewport_capture_test
add_executable(viewport_capture_test EXCLUDE_FROM_ALL tests/test_viewport_capture.cpp)
target_link_libraries(viewport_capture_test PRIVATE studio_surface studio_model)
target_include_directories(viewport_capture_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(viewport_capture_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME viewport_capture COMMAND viewport_capture_test)
add_dependencies(studio_tests viewport_capture_test)

add_executable(indexed_binding_test EXCLUDE_FROM_ALL tests/test_indexed_binding.cpp)
target_link_libraries(indexed_binding_test PRIVATE studio_surface studio_model)
target_include_directories(indexed_binding_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(indexed_binding_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME indexed_binding COMMAND indexed_binding_test)
add_dependencies(studio_tests indexed_binding_test)

# EVERY BOUND CONTROL ON A PAGE: does its binding resolve, and does its gate open? Three separate
# Output Setup failures were each found by hand before this existed.
add_executable(page_controls_test EXCLUDE_FROM_ALL tests/test_page_controls.cpp)
target_link_libraries(page_controls_test PRIVATE studio_surface studio_model)
target_include_directories(page_controls_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(page_controls_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(page_controls_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME page_controls COMMAND page_controls_test)
add_dependencies(studio_tests page_controls_test)

# The studio lays out the coil and injector output rows: what it writes, and which edits make it.
add_executable(engine_output_layout_test EXCLUDE_FROM_ALL tests/test_engine_output_layout.cpp)
target_link_libraries(engine_output_layout_test PRIVATE studio_model)
target_include_directories(engine_output_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(engine_output_layout_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(engine_output_layout_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME engine_output_layout COMMAND engine_output_layout_test)
add_dependencies(studio_tests engine_output_layout_test)

# An output pin's page offers what the pin and the engine can use: Function by pin, Cylinder by engine.
add_executable(output_row_pickers_test EXCLUDE_FROM_ALL tests/test_output_row_pickers.cpp)
target_link_libraries(output_row_pickers_test PRIVATE studio_surface studio_model)
target_include_directories(output_row_pickers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(output_row_pickers_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(output_row_pickers_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME output_row_pickers COMMAND output_row_pickers_test)
add_dependencies(studio_tests output_row_pickers_test)

# A string field inside a struct array (an output slot's name) reads and writes like any other field.
add_executable(config_string_test EXCLUDE_FROM_ALL tests/test_config_string.cpp)
target_link_libraries(config_string_test PRIVATE studio_model)
target_include_directories(config_string_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(config_string_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(config_string_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME config_string COMMAND config_string_test)
add_dependencies(studio_tests config_string_test)

# The Lua script field, resolved from whichever definition is loaded. Takes BOTH metas: the jayecu one
# from the repo, and an imported rusEFI .meta named by STUDIO_RUSEFI_META — the test skips that half
# when the variable is not set rather than failing on a machine that has never imported one.
add_executable(script_path_test EXCLUDE_FROM_ALL tests/test_script_path.cpp)
target_link_libraries(script_path_test PRIVATE studio_model)
target_include_directories(script_path_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(script_path_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(script_path_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME script_path COMMAND script_path_test)
add_dependencies(studio_tests script_path_test)

add_executable(table_restore_test EXCLUDE_FROM_ALL tests/test_table_restore.cpp)
target_link_libraries(table_restore_test PRIVATE studio_model)
target_include_directories(table_restore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_restore_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

add_executable(firing_presets_test EXCLUDE_FROM_ALL tests/test_firing_presets.cpp)
target_link_libraries(firing_presets_test PRIVATE studio_model)
target_include_directories(firing_presets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(firing_presets_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

add_executable(tailbank_test EXCLUDE_FROM_ALL tests/test_tailbank.cpp)
target_link_libraries(tailbank_test PRIVATE studio_surface studio_model)
target_include_directories(tailbank_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(tailbank_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# What ORDER does Tab walk a page in? Reading order, or the order it happened to be authored in?
#   cmake --build build --target tab_order_test && ./build/tab_order_test
add_executable(tab_order_test EXCLUDE_FROM_ALL
    tests/test_tab_order.cpp
    )
target_link_libraries(tab_order_test PRIVATE studio_surface studio_model)
target_include_directories(tab_order_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(tab_order_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME tab_order COMMAND tab_order_test)
add_dependencies(studio_tests tab_order_test)

# What does dragging a binding out of the dictionary DROP? A channel field is a LIST — the board's pool —
# and four of them had no control hint at all, so they landed on a spin box.
#   cmake --build build --target drop_widget_type_test && ./build/drop_widget_type_test
add_executable(drop_widget_type_test EXCLUDE_FROM_ALL
    tests/test_drop_widget_type.cpp)
target_link_libraries(drop_widget_type_test PRIVATE studio_model)
target_include_directories(drop_widget_type_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(drop_widget_type_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(drop_widget_type_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME drop_widget_type COMMAND drop_widget_type_test)
add_dependencies(studio_tests drop_widget_type_test)

# A caption the studio places is a Label — born with the Label type's Widget Defaults, like any other.
#   cmake --build build --target caption_defaults_test && ./build/caption_defaults_test
add_executable(caption_defaults_test EXCLUDE_FROM_ALL
    tests/test_caption_defaults.cpp
    )
target_link_libraries(caption_defaults_test PRIVATE studio_surface studio_model)
target_include_directories(caption_defaults_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(caption_defaults_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(caption_defaults_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME caption_defaults COMMAND caption_defaults_test)
add_dependencies(studio_tests caption_defaults_test)

# Does a Hyperlink behave as the Label it is? Caption editing is a capability the widget declares, not a
# type name the surface compares against.
#   cmake --build build --target caption_capability_test && ./build/caption_capability_test
add_executable(caption_capability_test EXCLUDE_FROM_ALL
    tests/test_caption_capability.cpp
    )
target_link_libraries(caption_capability_test PRIVATE studio_surface studio_model)
target_include_directories(caption_capability_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(caption_capability_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME caption_capability COMMAND caption_capability_test)
add_dependencies(studio_tests caption_capability_test)

# Does editing a panel IN PLACE keep what you edited? The scope carries the children in and out; the
# panel's own title had nowhere to be kept.
#   cmake --build build --target panel_scope_test && ./build/panel_scope_test
add_executable(panel_scope_test EXCLUDE_FROM_ALL
    tests/test_panel_scope.cpp
    )
target_link_libraries(panel_scope_test PRIVATE studio_surface studio_model)
target_include_directories(panel_scope_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(panel_scope_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(panel_scope_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME panel_scope COMMAND panel_scope_test)
add_dependencies(studio_tests panel_scope_test)

# Does a click go to the control you can SEE? Card layout stacks children and the condition picks one;
# the paint honoured that and the hit-test did not.
#   cmake --build build --target panel_focus_ring_test && ./build/panel_focus_ring_test
add_executable(panel_focus_ring_test EXCLUDE_FROM_ALL
    tests/test_panel_focus_ring.cpp
    )
target_link_libraries(panel_focus_ring_test PRIVATE studio_surface studio_model)
target_include_directories(panel_focus_ring_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(panel_focus_ring_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(panel_focus_ring_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME panel_focus_ring COMMAND panel_focus_ring_test)
add_dependencies(studio_tests panel_focus_ring_test)

#   cmake --build build --target table_scale_sweep_test && ./build/table_scale_sweep_test
add_executable(table_scale_sweep_test EXCLUDE_FROM_ALL tests/test_table_scale_sweep.cpp)
target_link_libraries(table_scale_sweep_test PRIVATE studio_surface studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(table_scale_sweep_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_compile_definitions(table_scale_sweep_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME table_scale_sweep COMMAND table_scale_sweep_test)
add_dependencies(studio_tests table_scale_sweep_test)

#   cmake --build build --target table_readout_test && ./build/table_readout_test
add_executable(table_readout_test EXCLUDE_FROM_ALL
    tests/test_table_readout.cpp
    )
target_link_libraries(table_readout_test PRIVATE studio_surface studio_model)
target_include_directories(table_readout_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(table_readout_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(table_readout_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME table_readout COMMAND table_readout_test)
add_dependencies(studio_tests table_readout_test)

#   cmake --build build --target wheel_validate_test && ./build/wheel_validate_test
add_executable(wheel_validate_test EXCLUDE_FROM_ALL tests/test_wheel_validate.cpp)
target_link_libraries(wheel_validate_test PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(wheel_validate_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
add_test(NAME wheel_validate COMMAND wheel_validate_test)
add_dependencies(studio_tests wheel_validate_test)

#   cmake --build build --target trigger_fit_test && ./build/trigger_fit_test
add_executable(trigger_fit_test EXCLUDE_FROM_ALL tests/test_trigger_fit.cpp)
target_link_libraries(trigger_fit_test PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(trigger_fit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_compile_definitions(trigger_fit_test PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME trigger_fit COMMAND trigger_fit_test)
add_dependencies(studio_tests trigger_fit_test)

# What the fitter makes of a directory of raw bench captures. Not a test — it asserts nothing and
# takes no expectations; it prints, so a sweep of the rig can be read and argued with.
#   cmake --build build --target fit_report && ./build/fit_report tests/fixtures/stim
add_executable(fit_report EXCLUDE_FROM_ALL tools/fit_report.cpp)
target_link_libraries(fit_report PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(fit_report PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)

#   cmake --build build --target card_input_test && ./build/card_input_test
add_executable(card_input_test EXCLUDE_FROM_ALL
    tests/test_card_input.cpp
    )
target_link_libraries(card_input_test PRIVATE studio_surface studio_model)
target_include_directories(card_input_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(card_input_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(card_input_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME card_input COMMAND card_input_test)
add_dependencies(studio_tests card_input_test)

# A J8HP (transmission controller) definition loads, is a TCU, and resolves; the product tokens the link accepts.
add_executable(j8hp_meta_test EXCLUDE_FROM_ALL tests/test_j8hp_meta.cpp)
target_link_libraries(j8hp_meta_test PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(j8hp_meta_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(j8hp_meta_test PRIVATE
    J8HP_META="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/j8hp_v1.meta"
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME j8hp_meta COMMAND j8hp_meta_test)
add_dependencies(studio_tests j8hp_meta_test)

add_executable(dict_dump EXCLUDE_FROM_ALL
    tests/dict_dump.cpp src/ui/DictionaryTree.cpp
    )
target_link_libraries(dict_dump PRIVATE studio_surface studio_model)
target_include_directories(dict_dump PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(dict_dump PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(dict_dump PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# Does the setting selector say what the ECU is actually set to? It hosts a framework combo box now, so
# the assertions are about the control's own items and selection.
#   cmake --build build --target setting_selector_test && ./build/setting_selector_test
add_executable(setting_selector_test EXCLUDE_FROM_ALL
    tests/test_setting_selector.cpp
    )
target_link_libraries(setting_selector_test PRIVATE studio_surface studio_model)
target_include_directories(setting_selector_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(setting_selector_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(setting_selector_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME setting_selector COMMAND setting_selector_test)
add_dependencies(studio_tests setting_selector_test)

# Does a search leave you in control of the tree? The picker is the dictionary's tree in a popup: what you
# open stays open, and a matched branch can still be collapsed while the query stands.
# What the expression BUILDER offers — functions and tables, per back end, all of them compiling.
#   cmake --build build --target expr_builder_tree_test && ./build/expr_builder_tree_test
add_executable(expr_builder_tree_test EXCLUDE_FROM_ALL
    tests/test_expr_builder_tree.cpp src/ui/DictionaryTree.cpp
    )
target_link_libraries(expr_builder_tree_test PRIVATE studio_surface studio_model)
target_include_directories(expr_builder_tree_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(expr_builder_tree_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(expr_builder_tree_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME expr_builder_tree COMMAND expr_builder_tree_test)

#   cmake --build build --target tree_filter_test && ./build/tree_filter_test
add_executable(tree_filter_test EXCLUDE_FROM_ALL
    tests/test_tree_filter.cpp src/ui/DictionaryTree.cpp
    )
target_link_libraries(tree_filter_test PRIVATE studio_surface studio_model)
target_include_directories(tree_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(tree_filter_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(tree_filter_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME tree_filter COMMAND tree_filter_test)
add_dependencies(studio_tests tree_filter_test)

# Does the field picker know every field there is? config() is the flat scalars and tables only; every
# array element field lives in configArrays() and was absent from the list a picker is built from.
#   cmake --build build --target config_paths_test && ./build/config_paths_test
add_executable(config_paths_test EXCLUDE_FROM_ALL
    tests/test_config_paths.cpp src/ui/DictionaryTree.cpp
    )
target_link_libraries(config_paths_test PRIVATE studio_surface studio_model)
target_include_directories(config_paths_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(config_paths_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(config_paths_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME config_paths COMMAND config_paths_test)
add_dependencies(studio_tests config_paths_test)

# Can two cylinders name one coil? A distributor runs every cylinder off one, wasted spark runs each
# companion pair off one — and a sensor pin is still exclusive.
#   cmake --build build --target pin_sharing_test && ./build/pin_sharing_test
add_executable(pin_sharing_test EXCLUDE_FROM_ALL
    tests/test_pin_sharing.cpp
    )
target_link_libraries(pin_sharing_test PRIVATE studio_surface studio_model)
target_include_directories(pin_sharing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(pin_sharing_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(pin_sharing_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME pin_sharing COMMAND pin_sharing_test)
add_dependencies(studio_tests pin_sharing_test)

# Can you reach the cells a 1-D strip cannot fit? Selection over the whole run, the view following it,
# and — the one that matters — a typed value landing in the cell the row names.
#   cmake --build build --target array1d_scroll_test && ./build/array1d_scroll_test
add_executable(array1d_scroll_test EXCLUDE_FROM_ALL
    tests/test_array1d_scroll.cpp
    )
target_link_libraries(array1d_scroll_test PRIVATE studio_surface studio_model)
target_include_directories(array1d_scroll_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(array1d_scroll_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(array1d_scroll_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME array1d_scroll COMMAND array1d_scroll_test)
add_dependencies(studio_tests array1d_scroll_test)

# Every addressable path in the ECU -> the address it resolves to. The contract a path-resolution
# rewrite must not break; see the file header for what is pinned and what deliberately is not.
#   cmake --build build --target locate_equiv_test && ./build/locate_equiv_test
add_executable(locate_equiv_test EXCLUDE_FROM_ALL
    tests/test_locate_equivalence.cpp)
target_link_libraries(locate_equiv_test PRIVATE studio_model)
target_include_directories(locate_equiv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(locate_equiv_test PRIVATE JFramework::platform)
target_compile_definitions(locate_equiv_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json"
    GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/locations.txt")
add_test(NAME locate_equiv COMMAND locate_equiv_test)
add_dependencies(studio_tests locate_equiv_test)

# Does what you type into a sensor calibration come back, and is what got stored what the firmware
# will read? Every type, at both ends of every range. See the file header.
#   cmake --build build --target sensor_roundtrip_test && ./build/sensor_roundtrip_test
add_executable(sensor_roundtrip_test EXCLUDE_FROM_ALL tests/test_sensor_roundtrip.cpp
    )
target_link_libraries(sensor_roundtrip_test PRIVATE studio_surface studio_model)
target_include_directories(sensor_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(sensor_roundtrip_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(sensor_roundtrip_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME sensor_roundtrip COMMAND sensor_roundtrip_test)
add_dependencies(studio_tests sensor_roundtrip_test)

# How many decimals does a table AXIS show? The channel it reads says so — see the file header.
#   cmake --build build --target axis_decimals_test && ./build/axis_decimals_test
add_executable(axis_decimals_test EXCLUDE_FROM_ALL tests/test_axis_decimals.cpp
    )
target_link_libraries(axis_decimals_test PRIVATE studio_surface studio_model)
target_include_directories(axis_decimals_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(axis_decimals_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(axis_decimals_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME axis_decimals COMMAND axis_decimals_test)
add_dependencies(studio_tests axis_decimals_test)

# What does a channel actually read? A generic input's descriptor is a union wire, not a sensor —
# see the file header for what that let through.
#   cmake --build build --target sensor_domain_test && ./build/sensor_domain_test
add_executable(sensor_domain_test EXCLUDE_FROM_ALL tests/test_sensor_domain.cpp)
target_link_libraries(sensor_domain_test PRIVATE studio_model)
target_include_directories(sensor_domain_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(sensor_domain_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(sensor_domain_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME sensor_domain COMMAND sensor_domain_test)
add_dependencies(studio_tests sensor_domain_test)

# WHICH interfaces a sensor may be read through. The catalogue declared it, the firmware masked it,
# and the meta published nothing — so every picker offered the whole enum. See the file header.
#   cmake --build build --target sensor_interfaces_test && ./build/sensor_interfaces_test
add_executable(sensor_interfaces_test EXCLUDE_FROM_ALL tests/test_sensor_interfaces.cpp)
target_link_libraries(sensor_interfaces_test PRIVATE studio_model)
target_include_directories(sensor_interfaces_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(sensor_interfaces_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(sensor_interfaces_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME sensor_interfaces COMMAND sensor_interfaces_test)
add_dependencies(studio_tests sensor_interfaces_test)

# Can a write land somewhere the field cannot legally hold? See the file header — for most of this
# codebase's life the answer was yes, silently, by integer wrap.
#   cmake --build build --target bounds_test && ./build/bounds_test
add_executable(bounds_test EXCLUDE_FROM_ALL tests/test_bounds.cpp
    )
target_link_libraries(bounds_test PRIVATE studio_surface studio_model)
target_include_directories(bounds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(bounds_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(bounds_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME bounds COMMAND bounds_test)
add_dependencies(studio_tests bounds_test)

# The LIVE shape of every table — bins per axis, and whether an optional axis is on. Not in the meta
# (it comes from the <axis>_n scalars), so the locations golden cannot pin it; this does.
#   cmake --build build --target table_dims_test && ./build/table_dims_test
add_executable(table_dims_test EXCLUDE_FROM_ALL tests/test_table_dims.cpp)
target_link_libraries(table_dims_test PRIVATE studio_model)
target_include_directories(table_dims_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_dims_test PRIVATE JFramework::platform)
target_compile_definitions(table_dims_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json"
    GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/table_dims.txt")
add_test(NAME table_dims COMMAND table_dims_test)
add_dependencies(studio_tests table_dims_test)


# The raw trigger log (0x27) -> the native cycle model: wire parsing, and what it does with a log
# that has no angle in it.
#   cmake --build build --target trigger_log_test && ./build/trigger_log_test
add_executable(trigger_log_test EXCLUDE_FROM_ALL
    tests/test_trigger_log.cpp
    src/model/TriggerLog.cpp
    src/model/EngineCycle.cpp)
target_include_directories(trigger_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME trigger_log COMMAND trigger_log_test)
add_dependencies(studio_tests trigger_log_test)

# The engine-cycle model: degrees, edges, wrap-aware dwell spans. Pure model, no framework dep.
#   cmake --build build --target engine_cycle_test && ./build/engine_cycle_test
add_executable(engine_cycle_test EXCLUDE_FROM_ALL
    tests/test_engine_cycle.cpp
    src/model/EngineCycle.cpp)
target_include_directories(engine_cycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The trigger wheel library: shipped wheels parsed from the meta, user wheels persisted on disk.
#   cmake --build build --target trigger_library_test && ./build/trigger_library_test
add_executable(trigger_library_test EXCLUDE_FROM_ALL
    tests/test_trigger_library.cpp
    )
target_link_libraries(trigger_library_test PRIVATE studio_model)
target_include_directories(trigger_library_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
    ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(trigger_library_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(trigger_library_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")

# Frame diagnostics: does the frame on screen deserve to be believed? Counts every lane and compares
# against the neighbouring frame, so a split or truncated cycle is named rather than drawn silently.
#   cmake --build build --target cycle_diagnostics_test && ./build/cycle_diagnostics_test
add_executable(cycle_diagnostics_test EXCLUDE_FROM_ALL
    tests/test_cycle_diagnostics.cpp
    src/model/CycleDiagnostics.cpp
    src/model/EngineCycle.cpp)
target_include_directories(cycle_diagnostics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The comms boundary: the firmware's 0x26 capture bytes -> the native cycle the view draws.
#   cmake --build build --target cycle_wire_test && ./build/cycle_wire_test
add_executable(cycle_wire_test EXCLUDE_FROM_ALL
    tests/test_cycle_wire.cpp
    src/model/CycleWire.cpp
    src/model/EngineCycle.cpp)
target_include_directories(cycle_wire_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# A rusEFI ECU's composite tooth log -> the same native cycle.
#   cmake --build build --target rusefi_cycle_test && ./build/rusefi_cycle_test
add_executable(rusefi_cycle_test EXCLUDE_FROM_ALL
    tests/test_rusefi_cycle.cpp
    src/model/RusefiCycle.cpp
    src/model/EngineCycle.cpp)
target_include_directories(rusefi_cycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The recorder's ring + cursor rules: what happens to a paused cursor when the ring rolls, what a
# step does at the ends, what shrinking the capacity keeps.
#   cmake --build build --target cycle_recorder_panel_test && ./build/cycle_recorder_panel_test
add_executable(cycle_recorder_panel_test EXCLUDE_FROM_ALL
    tests/test_cycle_recorder_panel.cpp
    src/ui/EngineCyclePanel.cpp
    src/ui/EngineCycleView.cpp
    src/model/CycleDiagnostics.cpp
    src/model/EngineCycle.cpp)
target_include_directories(cycle_recorder_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cycle_recorder_panel_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# The hover readout's COORDINATE CONTRACT: handleMouseMove is dispatched in absolute window
# coordinates, and the view must not offset them by its own origin. Placed at a non-zero origin on
# purpose — at (0,0) the right and wrong arithmetic agree, which is how the bug survived.
#   cmake --build build --target cycle_hover_test && ./build/cycle_hover_test
add_executable(cycle_hover_test EXCLUDE_FROM_ALL
    tests/test_cycle_hover.cpp
    src/ui/EngineCycleView.cpp
    src/model/CycleDiagnostics.cpp
    src/model/EngineCycle.cpp)
target_include_directories(cycle_hover_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cycle_hover_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# The inverse of tune_migrate's read side: raw image -> .tune dictionary, so a corrected image can
# become the studio's master copy again.
#   cmake --build build --target tune_from_image && ./build/tune_from_image <meta> <image> <out.tune>
add_executable(tune_from_image EXCLUDE_FROM_ALL tests/tune_from_image.cpp)
target_include_directories(tune_from_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../../generated)
target_link_libraries(tune_from_image PRIVATE studio_model)
target_link_libraries(tune_from_image PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})

# Bench harness for the NATIVE capture: real 0x26 bytes -> the shipped decoder -> the shipped
# diagnostics. Exits non-zero if any frame is untrustworthy, so a rig can gate on it.
#   cmake --build build --target cycle_diag_dump
add_executable(cycle_diag_dump EXCLUDE_FROM_ALL
    tests/cycle_diag_dump.cpp
    src/model/CycleDiagnostics.cpp
    src/model/CycleWire.cpp
    src/model/EngineCycle.cpp)
target_include_directories(cycle_diag_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Golden-value test for the pure SurfaceCamera world->screen transform (Stage 0 of the container migration).
#   cmake --build build --target surface_camera_test && ./build/surface_camera_test
add_executable(surface_camera_test EXCLUDE_FROM_ALL
    tests/test_surface_camera.cpp
    src/surface/SurfaceCamera.cpp
    src/surface/ContainerLayout.cpp)
target_include_directories(surface_camera_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/surface)
target_link_libraries(surface_camera_test PRIVATE JFramework::platform)

# The framework's widget TREE and widget LIFETIME: one child list written by every way of nesting a
# widget, effective visibility and scan exclusion following it, and focus traversal reaching a pane's
# by-value members with no per-pane hook to remember.
#   cmake --build build --target widget_ownership_test && ./build/widget_ownership_test
add_executable(widget_ownership_test EXCLUDE_FROM_ALL tests/test_widget_ownership.cpp)
target_link_libraries(widget_ownership_test PRIVATE JFramework::platform)
add_test(NAME widget_ownership COMMAND widget_ownership_test)
add_dependencies(studio_tests widget_ownership_test)

# Pasting a viewport: a NEW viewport on the SELECTED node, same size/position, with the copied viewport's
# widgets duplicated into that node's page. Pinned because it went wrong by hand three times.
#   cmake --build build --target viewport_paste_test && ./build/viewport_paste_test
# A viewport re-pointed at another page must show that page, all of it and only it — the mirrored-child
# cache is keyed by an element id that repeats across pages.
#   cmake --build build --target viewport_repoint_test && ./build/viewport_repoint_test
# A save must not delete pages the tree can still reach — pruneUnplacedPages runs inside buildDoc().
#   cmake --build build --target page_retention_test && ./build/page_retention_test
# One viewport per surface, showing whichever page is selected — and the two gates that decide whether a
# double-click can reach the page editor.
#   cmake --build build --target viewport_unbound_test && ./build/viewport_unbound_test
# A tab carries its own page, and arriving on one moves the tree to match.
#   cmake --build build --target tab_pages_test && ./build/tab_pages_test
add_executable(tab_pages_test EXCLUDE_FROM_ALL tests/test_tab_pages.cpp)
target_link_libraries(tab_pages_test PRIVATE studio_surface studio_model)
target_include_directories(tab_pages_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME tab_pages COMMAND tab_pages_test)

add_executable(viewport_unbound_test EXCLUDE_FROM_ALL tests/test_viewport_unbound.cpp)
target_link_libraries(viewport_unbound_test PRIVATE studio_surface studio_model)
target_include_directories(viewport_unbound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME viewport_unbound COMMAND viewport_unbound_test)

add_executable(page_retention_test EXCLUDE_FROM_ALL tests/test_page_retention.cpp)
target_link_libraries(page_retention_test PRIVATE studio_surface studio_model)
target_include_directories(page_retention_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME page_retention COMMAND page_retention_test)

add_executable(viewport_repoint_test EXCLUDE_FROM_ALL tests/test_viewport_repoint.cpp)
target_link_libraries(viewport_repoint_test PRIVATE studio_surface studio_model)
target_include_directories(viewport_repoint_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME viewport_repoint COMMAND viewport_repoint_test)

add_executable(viewport_paste_test EXCLUDE_FROM_ALL tests/test_viewport_paste.cpp
               )
target_link_libraries(viewport_paste_test PRIVATE studio_surface studio_model)
target_include_directories(viewport_paste_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(viewport_paste_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME viewport_paste COMMAND viewport_paste_test)
add_dependencies(studio_tests viewport_paste_test)

# What control a dictionary field drops as — a sensor selector is a PICKER, not a spin box.
#   cmake --build build --target control_kind_test && ./build/control_kind_test
# Every output personality the firmware ships compiles, and asks only for what a slot can hold.
add_executable(output_templates_test EXCLUDE_FROM_ALL tests/test_output_templates.cpp)
target_link_libraries(output_templates_test PRIVATE studio_model)
target_include_directories(output_templates_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(output_templates_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(output_templates_test PRIVATE
    META_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME output_templates COMMAND output_templates_test)
add_dependencies(studio_tests output_templates_test)

add_executable(control_kind_test EXCLUDE_FROM_ALL tests/test_control_kind.cpp)
target_link_libraries(control_kind_test PRIVATE studio_model)
target_include_directories(control_kind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(control_kind_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(control_kind_test PRIVATE
    META_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME control_kind COMMAND control_kind_test)
add_dependencies(studio_tests control_kind_test)

# Named bit groups (a sensor's diagnostics checks + severities) read and write as individual controls
# without disturbing their neighbours in the shared byte.
#   cmake --build build --target bit_groups_test && ./build/bit_groups_test
add_executable(bit_groups_test EXCLUDE_FROM_ALL tests/test_bit_groups.cpp)
target_link_libraries(bit_groups_test PRIVATE studio_model)
target_include_directories(bit_groups_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bit_groups_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME bit_groups COMMAND bit_groups_test)
add_dependencies(studio_tests bit_groups_test)

# Anchoring is per CANVAS, and an old layout with no anchor key must still centre.
#   cmake --build build --target canvas_anchor_test && ./build/canvas_anchor_test
# The flow's arithmetic — how many children fit across a width, and where the gutter goes.
#   cmake --build build --target flow_gap_test && ./build/flow_gap_test
add_executable(flow_gap_test EXCLUDE_FROM_ALL tests/test_flow_gap.cpp src/surface/ContainerLayout.cpp)
target_include_directories(flow_gap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(flow_gap_test PRIVATE JFramework::platform)
add_test(NAME flow_gap COMMAND flow_gap_test)
add_dependencies(studio_tests flow_gap_test)

add_executable(canvas_anchor_test EXCLUDE_FROM_ALL tests/test_canvas_anchor.cpp)
target_link_libraries(canvas_anchor_test PRIVATE studio_model)
target_include_directories(canvas_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(canvas_anchor_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME canvas_anchor COMMAND canvas_anchor_test)
add_dependencies(studio_tests canvas_anchor_test)

# A GROUP arranges as ONE item — align/distribute/spacing move it by a single delta.
#   cmake --build build --target arrange_groups_test && ./build/arrange_groups_test
add_executable(arrange_groups_test EXCLUDE_FROM_ALL tests/test_arrange_groups.cpp
               )
target_link_libraries(arrange_groups_test PRIVATE studio_surface studio_model)

#   cmake --build build --target reflow_floor_test && ./build/reflow_floor_test
add_executable(reflow_floor_test EXCLUDE_FROM_ALL tests/test_reflow_floor.cpp
               )
target_link_libraries(reflow_floor_test PRIVATE studio_surface studio_model)
target_include_directories(arrange_groups_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(arrange_groups_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME arrange_groups COMMAND arrange_groups_test)
add_dependencies(studio_tests arrange_groups_test)

# EVERY WAY OF WRITING A WIDGET ADDRESS names the same widget.
#   cmake --build build --target widget_addr_test && ./build/widget_addr_test
add_executable(widget_addr_test EXCLUDE_FROM_ALL tests/test_widget_addr.cpp)
target_include_directories(widget_addr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(widget_addr_test PRIVATE JFramework::platform)
add_test(NAME widget_addr COMMAND widget_addr_test)
add_dependencies(studio_tests widget_addr_test)

# A LABEL CAN BE THE UNIT — Show Unit prints the binding's display unit, and follows it.
#   cmake --build build --target label_unit_test && ./build/label_unit_test
add_executable(label_unit_test EXCLUDE_FROM_ALL tests/test_label_unit.cpp
               )
target_link_libraries(label_unit_test PRIVATE studio_surface studio_model)
target_include_directories(label_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(label_unit_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME label_unit COMMAND label_unit_test)
add_dependencies(studio_tests label_unit_test)

# A RULE'S `value` MEANS WHAT THE CONTROL SHOWS — display units, not the raw bus reading.
#   cmake --build build --target rule_units_test && ./build/rule_units_test
add_executable(rule_units_test EXCLUDE_FROM_ALL tests/test_rule_units.cpp
               )
target_link_libraries(rule_units_test PRIVATE studio_surface studio_model)
target_include_directories(rule_units_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(rule_units_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
target_link_libraries(rule_units_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME rule_units COMMAND rule_units_test)
add_dependencies(studio_tests rule_units_test)

# AN IMAGE IS DECODED ONCE — and a bad path costs one attempt, not one per frame.
#   cmake --build build --target image_cache_test && ./build/image_cache_test
add_executable(image_cache_test EXCLUDE_FROM_ALL tests/test_image_cache.cpp)
target_link_libraries(image_cache_test PRIVATE studio_model)
target_include_directories(image_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(image_cache_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME image_cache COMMAND image_cache_test)
add_dependencies(studio_tests image_cache_test)

# A BAND CAN BE A CONDITION — `when` expressions, and the `value` keyword.
#   cmake --build build --target range_when_test && ./build/range_when_test
add_executable(range_when_test EXCLUDE_FROM_ALL tests/test_range_when.cpp
               )
target_link_libraries(range_when_test PRIVATE studio_surface studio_model)
target_include_directories(range_when_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(range_when_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME range_when COMMAND range_when_test)
add_dependencies(studio_tests range_when_test)

# PADDING NARROWS A WIDGET, NEVER ERASES IT — the inset is capped at what leaves content.
#   cmake --build build --target content_rect_test && ./build/content_rect_test
add_executable(content_rect_test EXCLUDE_FROM_ALL tests/test_content_rect.cpp
               )
target_link_libraries(content_rect_test PRIVATE studio_surface studio_model)
target_include_directories(content_rect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(content_rect_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME content_rect COMMAND content_rect_test)
add_dependencies(studio_tests content_rect_test)

# VALUE RANGES REACH THE BAR — asserted against what the render actually painted.
#   cmake --build build --target bar_ranges_test && ./build/bar_ranges_test
add_executable(bar_ranges_test EXCLUDE_FROM_ALL tests/test_bar_ranges.cpp
               )
target_link_libraries(bar_ranges_test PRIVATE studio_surface studio_model)
target_include_directories(bar_ranges_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bar_ranges_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME bar_ranges COMMAND bar_ranges_test)
add_dependencies(studio_tests bar_ranges_test)

# VALUE RANGES REACH THE DIAL — its own named colours resolve through the band cascade.
#   cmake --build build --target dial_ranges_test && ./build/dial_ranges_test
add_executable(dial_ranges_test EXCLUDE_FROM_ALL tests/test_dial_ranges.cpp
               )
target_link_libraries(dial_ranges_test PRIVATE studio_surface studio_model)
target_include_directories(dial_ranges_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dial_ranges_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME dial_ranges COMMAND dial_ranges_test)
add_dependencies(studio_tests dial_ranges_test)

# PER-WIDGET STATE IS KEYED BY UID — two widgets sharing an element id must not share state.
#   cmake --build build --target widget_state_keys_test && ./build/widget_state_keys_test
add_executable(widget_state_keys_test EXCLUDE_FROM_ALL tests/test_widget_state_keys.cpp
               )
target_link_libraries(widget_state_keys_test PRIVATE studio_surface studio_model)
target_include_directories(widget_state_keys_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(widget_state_keys_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME widget_state_keys COMMAND widget_state_keys_test)
add_dependencies(studio_tests widget_state_keys_test)

# A CELL OP EDITS THE PLANE YOU ARE LOOKING AT — copy/paste/smooth must not always hit plane 0.
#   cmake --build build --target table_planes_test && ./build/table_planes_test
add_executable(table_planes_test EXCLUDE_FROM_ALL tests/test_table_planes.cpp
               )
target_link_libraries(table_planes_test PRIVATE studio_surface studio_model)
target_include_directories(table_planes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_planes_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(table_planes_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME table_planes COMMAND table_planes_test)
add_dependencies(studio_tests table_planes_test)

# THE TABLE WIDGET READS WHAT THE CACHE WROTE — one cell-address rule, not one per caller.
#   cmake --build build --target table_addressing_test && ./build/table_addressing_test
add_executable(table_addressing_test EXCLUDE_FROM_ALL tests/test_table_addressing.cpp
               )
target_link_libraries(table_addressing_test PRIVATE studio_surface studio_model)
target_include_directories(table_addressing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_addressing_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(table_addressing_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME table_addressing COMMAND table_addressing_test)
add_dependencies(studio_tests table_addressing_test)

# A TABLE IS ITS CELLS *AND* ITS AXES — a connect-point restore must bring back both.
#   cmake --build build --target table_restore_axes_test && ./build/table_restore_axes_test
add_executable(table_restore_axes_test EXCLUDE_FROM_ALL tests/test_table_restore_axes.cpp)
target_link_libraries(table_restore_axes_test PRIVATE studio_model)
target_include_directories(table_restore_axes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_restore_axes_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(table_restore_axes_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME table_restore_axes COMMAND table_restore_axes_test)
add_dependencies(studio_tests table_restore_axes_test)

# A TABLE'S BIN COUNT IS A RESIZE, NOT A NUMBER — the generic scalar write must not re-stride a grid.
#   cmake --build build --target axis_stride_test && ./build/axis_stride_test
add_executable(axis_stride_test EXCLUDE_FROM_ALL tests/test_axis_stride.cpp)
target_link_libraries(axis_stride_test PRIVATE studio_model)
target_include_directories(axis_stride_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(axis_stride_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(axis_stride_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME axis_stride COMMAND axis_stride_test)
add_dependencies(studio_tests axis_stride_test)

# A UID NAMES EXACTLY ONE WIDGET — a copied panel's CHILDREN get fresh ones too.
#   cmake --build build --target child_uids_test && ./build/child_uids_test
add_executable(child_uids_test EXCLUDE_FROM_ALL tests/test_child_uids.cpp)
target_link_libraries(child_uids_test PRIVATE studio_model)
target_include_directories(child_uids_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(child_uids_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME child_uids COMMAND child_uids_test)
add_dependencies(studio_tests child_uids_test)

# SCALE ZONES REACH THE GAUGE FACE — the dial's arc and the scale's ticks, both asserted against the render.
#   cmake --build build --target gauge_zones_test && ./build/gauge_zones_test
add_executable(gauge_zones_test EXCLUDE_FROM_ALL tests/test_gauge_zones.cpp
               )
target_link_libraries(gauge_zones_test PRIVATE studio_surface studio_model)
target_include_directories(gauge_zones_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gauge_zones_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME gauge_zones COMMAND gauge_zones_test)
add_dependencies(studio_tests gauge_zones_test)

# SCALE ZONES — the gauge-face spans the dial and the scale both paint from.
#   cmake --build build --target scale_zones_test && ./build/scale_zones_test
add_executable(scale_zones_test EXCLUDE_FROM_ALL tests/test_scale_zones.cpp)
target_include_directories(scale_zones_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME scale_zones COMMAND scale_zones_test)
add_dependencies(studio_tests scale_zones_test)

# TYPING A CAPTION IN PLACE KEEPS WHAT YOU TYPED — through the resize churn, a re-source and a save.
#   cmake --build build --target caption_edit_test && ./build/caption_edit_test
add_executable(caption_edit_test EXCLUDE_FROM_ALL tests/test_caption_edit.cpp
               )
target_link_libraries(caption_edit_test PRIVATE studio_surface studio_model)
target_include_directories(caption_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(caption_edit_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(caption_edit_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME caption_edit COMMAND caption_edit_test)
add_dependencies(studio_tests caption_edit_test)

# CLICKING IN AN OPEN CAPTION PUTS THE CARET WHERE YOU CLICKED.
#   cmake --build build --target caption_caret_test && ./build/caption_caret_test
add_executable(caption_caret_test EXCLUDE_FROM_ALL tests/test_caption_caret.cpp
               )
target_link_libraries(caption_caret_test PRIVATE studio_surface studio_model)
target_include_directories(caption_caret_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(caption_caret_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(caption_caret_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json"
                           TEST_FONT="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
add_test(NAME caption_caret COMMAND caption_caret_test)
add_dependencies(studio_tests caption_caret_test)

# A BUTTON THAT WORKS OUT A SETTING — the Calibrate that turns Hz and a known speed into pulses/km.
#   cmake --build build --target action_button_test && ./build/action_button_test
add_executable(action_button_test EXCLUDE_FROM_ALL tests/test_action_button.cpp
               )
target_link_libraries(action_button_test PRIVATE studio_surface studio_model)
target_include_directories(action_button_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(action_button_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(action_button_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME action_button COMMAND action_button_test)
add_dependencies(studio_tests action_button_test)

# A CALIBRATION LEAVES THE STUDIO AND COMES BACK UNCHANGED — the table CSV round trip.
#   cmake --build build --target table_file_test && ./build/table_file_test
add_executable(table_file_test EXCLUDE_FROM_ALL tests/test_table_file.cpp
               )
target_link_libraries(table_file_test PRIVATE studio_surface studio_model)
target_include_directories(table_file_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(table_file_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(table_file_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json"
                           TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME table_file COMMAND table_file_test)
add_dependencies(studio_tests table_file_test)

# A BAND BELONGS TO THE CHANNEL — one threshold, obeyed by every control reading it.
#   cmake --build build --target channel_prefs_test && ./build/channel_prefs_test
add_executable(channel_prefs_test EXCLUDE_FROM_ALL tests/test_channel_prefs.cpp
               )
target_link_libraries(channel_prefs_test PRIVATE studio_surface studio_model)
target_include_directories(channel_prefs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(channel_prefs_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(channel_prefs_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME channel_prefs COMMAND channel_prefs_test)
add_dependencies(studio_tests channel_prefs_test)

# A THRESHOLD SAYS WHAT IT IS IN — "200ms" and ".2s" are one band, whatever unit the reading is shown in.
#   cmake --build build --target unit_literals_test && ./build/unit_literals_test
add_executable(unit_literals_test EXCLUDE_FROM_ALL tests/test_unit_literals.cpp
               )
target_link_libraries(unit_literals_test PRIVATE studio_surface studio_model)
target_include_directories(unit_literals_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(unit_literals_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME unit_literals COMMAND unit_literals_test)
add_dependencies(studio_tests unit_literals_test)

# THE LEGEND'S EYE hides one trace and leaves the others their colour, place and range.
#   cmake --build build --target trace_legend_test && ./build/trace_legend_test
add_executable(trace_legend_test EXCLUDE_FROM_ALL tests/test_trace_legend.cpp
               )
target_link_libraries(trace_legend_test PRIVATE studio_surface studio_model)
target_include_directories(trace_legend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(trace_legend_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(trace_legend_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json"
                           TEST_FONT="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
add_test(NAME trace_legend COMMAND trace_legend_test)
add_dependencies(studio_tests trace_legend_test)

# ONLY THE TAB IN FRONT ANSWERS A RIGHT-CLICK — a closed surface tab must not serve its own menu.
#   cmake --build build --target closed_tab_test && ./build/closed_tab_test
add_executable(closed_tab_test EXCLUDE_FROM_ALL tests/test_closed_tab.cpp
               )
target_link_libraries(closed_tab_test PRIVATE studio_surface studio_model)
target_include_directories(closed_tab_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(closed_tab_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME closed_tab COMMAND closed_tab_test)
add_dependencies(studio_tests closed_tab_test)

# SELECTING A NODE GOES TO A TAB THAT CAN SHOW IT — and a focused workspace keeps its own selections.
#   cmake --build build --target node_follows_tab_test && ./build/node_follows_tab_test
add_executable(node_follows_tab_test EXCLUDE_FROM_ALL tests/test_node_follows_tab.cpp
               )
target_link_libraries(node_follows_tab_test PRIVATE studio_surface studio_model)
target_include_directories(node_follows_tab_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(node_follows_tab_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME node_follows_tab COMMAND node_follows_tab_test)
add_dependencies(studio_tests node_follows_tab_test)

# A LABEL WRAPS TO ITS BOX — word breaks, in-word breaks, and the offsets the caret reads.
#   cmake --build build --target label_wrap_test && ./build/label_wrap_test
add_executable(label_wrap_test EXCLUDE_FROM_ALL tests/test_label_wrap.cpp
               )
target_link_libraries(label_wrap_test PRIVATE studio_surface studio_model)
target_include_directories(label_wrap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(label_wrap_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME label_wrap COMMAND label_wrap_test)
add_dependencies(studio_tests label_wrap_test)

# VALUE RANGES REACH A HOSTED CONTROL — on the role it paints from, not just Base.
#   cmake --build build --target hosted_ranges_test && ./build/hosted_ranges_test
add_executable(hosted_ranges_test EXCLUDE_FROM_ALL tests/test_hosted_ranges.cpp
               )
target_link_libraries(hosted_ranges_test PRIVATE studio_surface studio_model)
target_include_directories(hosted_ranges_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hosted_ranges_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME hosted_ranges COMMAND hosted_ranges_test)
add_dependencies(studio_tests hosted_ranges_test)

# A LINK CANNOT REACH A HIDDEN PAGE — a Label's Link answers to the target's visibility condition.
#   cmake --build build --target link_visibility_test && ./build/link_visibility_test
add_executable(link_visibility_test EXCLUDE_FROM_ALL tests/test_link_visibility.cpp)
target_include_directories(link_visibility_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(link_visibility_test PRIVATE JFramework::platform)
add_test(NAME link_visibility COMMAND link_visibility_test)
add_dependencies(studio_tests link_visibility_test)

# A GROUP HAS A POSITION — the inspector's X/Y are its origin, and writing them translates it.
#   cmake --build build --target group_origin_test && ./build/group_origin_test
add_executable(group_origin_test EXCLUDE_FROM_ALL tests/test_group_origin.cpp
               )
target_link_libraries(group_origin_test PRIVATE studio_surface studio_model)
target_include_directories(group_origin_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(group_origin_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME group_origin COMMAND group_origin_test)
add_dependencies(studio_tests group_origin_test)

# A pin belongs to one sensor: which pins the picker greys, and why (by NAME, enabled only, not your own).
#   cmake --build build --target pin_conflict_test && ./build/pin_conflict_test
add_executable(pin_conflict_test EXCLUDE_FROM_ALL tests/test_pin_conflict.cpp
               )
target_link_libraries(pin_conflict_test PRIVATE studio_surface studio_model)
target_include_directories(pin_conflict_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pin_conflict_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
# The meta by ABSOLUTE path. It used to guess with two relative candidates and SKIP — returning 0 —
# when neither hit, so running the binary from anywhere but the build directory reported success
# having tested nothing.
target_compile_definitions(pin_conflict_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME pin_conflict COMMAND pin_conflict_test)
add_dependencies(studio_tests pin_conflict_test)

# Bit-packed config fields: extraction on read, read-modify-write on write.
add_executable(bit_fields_test EXCLUDE_FROM_ALL tests/test_bit_fields.cpp)
target_link_libraries(bit_fields_test PRIVATE studio_model)
target_include_directories(bit_fields_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bit_fields_test PRIVATE JFramework::platform)

# A picker gated by a sibling: when the gate moves the stored index addresses a different list, so it is
# stale even though it is still in range. Restored to the field's own (per-element) schema default.
add_executable(gate_invalidation_test EXCLUDE_FROM_ALL tests/test_gate_invalidation.cpp)
target_link_libraries(gate_invalidation_test PRIVATE studio_model)
target_include_directories(gate_invalidation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gate_invalidation_test PRIVATE JFramework::platform)

# Releasing a pin must return it to the pool — by clearing the field, or by disabling its holder.
add_executable(pin_release_test EXCLUDE_FROM_ALL tests/test_pin_release.cpp)
target_link_libraries(pin_release_test PRIVATE studio_surface studio_model)
target_include_directories(pin_release_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pin_release_test PRIVATE JFramework::platform)

# The sensor page's raw input readout: its "[$~]" binding resolves to a SIGIL, which name-keyed lookups
# must still resolve, or the readout loses its unit and the precision that unit carries.
add_executable(raw_input_readout_test EXCLUDE_FROM_ALL tests/test_raw_input_readout.cpp)
target_link_libraries(raw_input_readout_test PRIVATE studio_surface studio_model)
target_include_directories(raw_input_readout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(raw_input_readout_test PRIVATE JFramework::platform)

# The imported .ini's [Menu] + [UserDefined] parser. With no argument it runs the built-in syntax cases;
# given a TunerStudio-format .ini it also reports what the dashboard import makes of that file.
#   cmake --build build --target ts_dashboard_test && ./build/ts_dashboard_test [path/to.ini]
add_executable(ts_dashboard_test EXCLUDE_FROM_ALL tests/test_ts_dashboard.cpp)
target_link_libraries(ts_dashboard_test PRIVATE studio_model)
target_include_directories(ts_dashboard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ts_dashboard_test PRIVATE JFramework::platform)

# Text is ONE shape in the meta (scalar + datatype ASCII) — asserted through BOTH producers, since one
# producer diverging is what made native text fields decode as numbers.
#   cmake --build build --target meta_text_test && ./build/meta_text_test
add_executable(meta_text_test EXCLUDE_FROM_ALL tests/test_meta_text.cpp)
target_link_libraries(meta_text_test PRIVATE studio_model)
target_include_directories(meta_text_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(meta_text_test PRIVATE JFramework::platform)
add_test(NAME meta_text COMMAND meta_text_test)
add_dependencies(studio_tests meta_text_test)

# Which firmware kit is offered for an ECU: half-copied kits skipped, newest usable wins, a kit needing a
# newer studio passed over, downloaded beats shipped on a tie.
#   cmake --build build --target firmware_kits_test && ./build/firmware_kits_test
add_executable(firmware_kits_test EXCLUDE_FROM_ALL tests/test_firmware_kits.cpp)
target_link_libraries(firmware_kits_test PRIVATE studio_model JFramework::platform)
add_test(NAME firmware_kits COMMAND firmware_kits_test)
add_dependencies(studio_tests firmware_kits_test)

# The DFU write against a simulated STM32 ROM bootloader: image placement, which sectors are erased,
# the device's block size, read-back, and errors. No hardware.
#   cmake --build build --target dfu_test && ./build/dfu_test
add_executable(dfu_test EXCLUDE_FROM_ALL tests/test_dfu.cpp)
target_link_libraries(dfu_test PRIVATE studio_model JFramework::platform)
add_test(NAME dfu COMMAND dfu_test)
add_dependencies(studio_tests dfu_test)
# Read-only probe of the real bootloader (hardware; not part of the suite).
#   cmake --build build --target dfu_probe && ./build/dfu_probe
add_executable(dfu_probe EXCLUDE_FROM_ALL tests/dfu_probe.cpp)
target_link_libraries(dfu_probe PRIVATE studio_model JFramework::platform)
# Write an image to an ECU waiting in DFU, with the studio's engine (recovery / hardware check).
#   cmake --build build --target dfu_write && ./build/dfu_write <image.bin>
add_executable(dfu_write EXCLUDE_FROM_ALL tests/dfu_write.cpp)
target_link_libraries(dfu_write PRIVATE studio_model JFramework::platform)
target_link_libraries(widget_ownership_test PRIVATE JFramework::platform)

# The TS write path: a payload larger than one blockingFactor block must still land, whole and in order.
#   cmake --build build --target ts_write_chunk_test && ./build/ts_write_chunk_test
add_executable(ts_write_chunk_test EXCLUDE_FROM_ALL tests/test_ts_write_chunking.cpp)
target_include_directories(ts_write_chunk_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ts_write_chunk_test PRIVATE util)

# The < > arrows on an imported dialog walk THAT page's edits, not a global history.
#   cmake --build build --target scoped_undo_test && ./build/scoped_undo_test
add_executable(scoped_undo_test EXCLUDE_FROM_ALL tests/test_scoped_undo.cpp)
target_link_libraries(scoped_undo_test PRIVATE studio_model)
target_include_directories(scoped_undo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(scoped_undo_test PRIVATE JFramework::platform)
target_compile_definitions(scoped_undo_test PRIVATE
    FIXTURE_META="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test-meta.json")

# Where an inserted axis bin lands — the position comes from the VALUE, not the cursor.
#   cmake --build build --target axis_insert_test && ./build/axis_insert_test
add_executable(axis_insert_test EXCLUDE_FROM_ALL tests/test_axis_insert.cpp)
target_link_libraries(axis_insert_test PRIVATE studio_model)
target_include_directories(axis_insert_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(axis_insert_test PRIVATE JFramework::platform)
target_compile_definitions(axis_insert_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME axis_insert COMMAND axis_insert_test)
add_dependencies(studio_tests axis_insert_test)

# A switched-off optional axis draws no breakpoint strip (and no blank corner).
#   cmake --build build --target axis_disabled_strip_test && ./build/axis_disabled_strip_test
add_executable(axis_disabled_strip_test EXCLUDE_FROM_ALL tests/test_axis_disabled_strip.cpp
     ${STUDIO_SURFACE_SRC} ${STUDIO_WIDGET_SRC} src/app/Resources.cpp)
target_link_libraries(axis_disabled_strip_test PRIVATE studio_model)
target_include_directories(axis_disabled_strip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(axis_disabled_strip_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(axis_disabled_strip_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME axis_disabled_strip COMMAND axis_disabled_strip_test)
add_dependencies(studio_tests axis_disabled_strip_test)

# A tune stores its references (signal selectors, enum options) BY NAME, not by ordinal.
#   cmake --build build --target tune_refs_test && ./build/tune_refs_test
add_executable(tune_refs_test EXCLUDE_FROM_ALL tests/test_tune_refs_by_name.cpp)
target_link_libraries(tune_refs_test PRIVATE studio_model)
target_include_directories(tune_refs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tune_refs_test PRIVATE JFramework::platform)
target_compile_definitions(tune_refs_test PRIVATE REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME tune_refs COMMAND tune_refs_test)
add_dependencies(studio_tests tune_refs_test)



# "Why is it sluggish" — what one frame of config reads costs, against the real meta. Not a ctest: it
# is a measurement, not a pass/fail, and the numbers it should beat are recorded in its header.
add_executable(bench_config_reads EXCLUDE_FROM_ALL tests/bench_config_reads.cpp)
target_link_libraries(bench_config_reads PRIVATE studio_model)
target_include_directories(bench_config_reads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bench_config_reads PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})




# THROWAWAY PROBES DO NOT BELONG IN THE BUILD. Three add_executable entries here pointed at .cpp files in
# a temp directory of a session that ended weeks ago, so every `cmake -S . -B build` failed outright with
# "cannot find source file" — the build only kept working because nothing had needed to regenerate since.
# A one-off experiment gets compiled by hand; it does not get a target.
add_dependencies(studio_tests expr_builder_tree_test)

# The VE autotuner's arithmetic — which cell a reading is credited to once transport delay is taken
# into account, whether the ECU's own correction is folded in, and whether a filter meant to reject a
# tip-in sees the tip-in rather than the calm that follows it.
#   cmake --build build --target autotune_test && ./build/autotune_test
add_executable(autotune_test EXCLUDE_FROM_ALL
    tests/test_autotune.cpp
    )
target_link_libraries(autotune_test PRIVATE studio_model)
target_include_directories(autotune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(autotune_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
target_link_libraries(autotune_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME autotune COMMAND autotune_test)
add_dependencies(studio_tests autotune_test)

# The autotuner's contract as a TunerStudio/rusEFI .ini states it: a derived ego channel that a field
# parser drops, and a percentage basis that a multiplier reading would scale by a hundred.
#   cmake --build build --target ts_autotune_test && ./build/ts_autotune_test
add_executable(ts_autotune_test EXCLUDE_FROM_ALL tests/test_ts_autotune.cpp)
target_link_libraries(ts_autotune_test PRIVATE studio_model)
target_include_directories(ts_autotune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ts_autotune_test PRIVATE BUILD_TMP="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ts_autotune_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME ts_autotune COMMAND ts_autotune_test)
add_dependencies(studio_tests ts_autotune_test)

# [Datalog] from an imported definition becomes the recorder's default channel set.
#   cmake --build build --target ts_datalog_test && ./build/ts_datalog_test
add_executable(ts_datalog_test EXCLUDE_FROM_ALL tests/test_ts_datalog.cpp)
target_link_libraries(ts_datalog_test PRIVATE studio_model)
target_include_directories(ts_datalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ts_datalog_test PRIVATE BUILD_TMP="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ts_datalog_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME ts_datalog COMMAND ts_datalog_test)
add_dependencies(studio_tests ts_datalog_test)

# The autotuner's grid has to stay the fuel table's grid: a moved breakpoint or an inserted bin makes
# every accumulated record describe a cell that no longer exists, and an Apply would write the proposal
# into cells it was never measured in.
#   cmake --build build --target autotune_grid_test && ./build/autotune_grid_test
add_executable(autotune_grid_test EXCLUDE_FROM_ALL
    tests/test_autotune_grid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/AutotunePanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/AutotuneView.cpp
    )
target_link_libraries(autotune_grid_test PRIVATE studio_model)
target_include_directories(autotune_grid_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(autotune_grid_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(autotune_grid_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME autotune_grid COMMAND autotune_grid_test)
add_dependencies(studio_tests autotune_grid_test)

# The confirmation dialog's text: it must wrap without a font atlas, because that is the state it is
# built in — and a question laid out as one clipped line is a question the user cannot read.
#   cmake --build build --target choice_dialog_test && ./build/choice_dialog_test
add_executable(choice_dialog_test EXCLUDE_FROM_ALL tests/test_choice_dialog.cpp)
target_link_libraries(choice_dialog_test PRIVATE studio_model)
target_include_directories(choice_dialog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(choice_dialog_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
add_test(NAME choice_dialog COMMAND choice_dialog_test)
add_dependencies(studio_tests choice_dialog_test)

# Axis headings that are names: the schema declares them, codegen emits them, the model carries them.
# Each of those links was at some point the one that was missing.
#   cmake --build build --target axis_labels_test && ./build/axis_labels_test
add_executable(axis_labels_test EXCLUDE_FROM_ALL tests/test_axis_labels.cpp)
target_link_libraries(axis_labels_test PRIVATE studio_model)
target_include_directories(axis_labels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(axis_labels_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(axis_labels_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME axis_labels COMMAND axis_labels_test)
add_dependencies(studio_tests axis_labels_test)

# What differs between two tunes — the evidence behind "keep the ECU's tune or push yours", which was
# a choice made blind until there was a report.
#   cmake --build build --target tune_diff_test && ./build/tune_diff_test
add_executable(tune_diff_test EXCLUDE_FROM_ALL tests/test_tune_diff.cpp)
target_link_libraries(tune_diff_test PRIVATE studio_model studio_surface)
target_include_directories(tune_diff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tune_diff_test PRIVATE JFramework::platform ${STUDIO_TLS_LIBS})
target_compile_definitions(tune_diff_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME tune_diff COMMAND tune_diff_test)
add_dependencies(studio_tests tune_diff_test)

# TESTS THAT WERE BUILT BUT NEVER RUN. Each of these had a target and no add_test, so ctest counted
# around them and the suite reported green over code nothing was checking. Registered here, together,
# so the list is one place. (firing_presets_test, tailbank_test and trigger_library_test are still out:
# they no longer compile against the Raw value type and PresetOptions::Pair, and need bringing up to date.)
foreach(t trigger_core_test tune_crc_watch_test runs_test cache_blocks_test expr_compiler_test expr_tune_test expr_widget_test output_readout_test wiring_thresh_test expr_unified_test table_restore_test engine_cycle_test cycle_diagnostics_test cycle_wire_test rusefi_cycle_test cycle_recorder_panel_test cycle_hover_test surface_camera_test reflow_floor_test bit_fields_test gate_invalidation_test pin_release_test raw_input_readout_test ts_dashboard_test ts_write_chunk_test scoped_undo_test)
    string(REGEX REPLACE "_test$" "" name ${t})
    add_test(NAME ${name} COMMAND ${t})
    add_dependencies(studio_tests ${t})
endforeach()

# The help pages are HTML generated from the definition (HelpPages).
#   cmake --build build --target help_pages_test && ./build/help_pages_test
add_executable(help_pages_test EXCLUDE_FROM_ALL tests/test_help_pages.cpp src/app/HelpPages.cpp src/app/Resources.cpp)
target_link_libraries(help_pages_test PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(help_pages_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(help_pages_test PRIVATE
    REAL_META="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tuneit-meta.json")
add_test(NAME help_pages COMMAND help_pages_test)
add_dependencies(studio_tests help_pages_test)

# A released version's descriptor and dashboard, fetched from a release served locally: the right release,
# checksums, the ECU's layout, a missing version (FirmwareFetch::fetchVersion).
#   cmake --build build --target firmware_version_fetch_test && ./build/firmware_version_fetch_test
add_executable(firmware_version_fetch_test EXCLUDE_FROM_ALL tests/test_firmware_version_fetch.cpp src/app/FirmwareFetch.cpp)
target_link_libraries(firmware_version_fetch_test PRIVATE studio_model JFramework::platform ${STUDIO_TLS_LIBS})
target_include_directories(firmware_version_fetch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_test(NAME firmware_version_fetch COMMAND firmware_version_fetch_test)
add_dependencies(studio_tests firmware_version_fetch_test)
