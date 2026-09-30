cmake_minimum_required(VERSION 3.22)
project(life_sim_bench LANGUAGES C CXX)


# Every target links libstdc++ statically, not just the GUI one. MSYS2 and
# Git-for-Windows both ship a libstdc++-6.dll, and a dynamically linked binary
# can bind to the wrong one and segfault constructing an ifstream â€” a failure
# that appears ONLY in optimised builds and looks exactly like UB in your code.
if(MINGW)
    set(STATIC_CXX -static -static-libgcc -static-libstdc++)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# -- Box2D 3.1.1 (Particle Collision Lab) ------------------------------------
# Vendored at native/third_party/box2d (tag v3.1.1). Built as a static library
# with this toolchain; samples/tests from upstream are not part of the tree.
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/box2d EXCLUDE_FROM_ALL)

# -- Jolt 5.6.0 (Particle Lab 3D) --------------------------------------------
# Default: analytic SphereWorld via jolt_stub INTERFACE (no upstream compile).
# Optional: -DBENCH_WITH_JOLT=ON after native/scripts/fetch_jolt_pin.ps1.
# Do not confuse with FlyGym/MuJoCo (separate world; no shared contacts).
add_subdirectory(third_party/jolt EXCLUDE_FROM_ALL)

# Every translation unit that pulls in registry.hpp (and therefore collision.hpp)
# needs the Box2D symbols. Linking the static archive into all native targets
# keeps that simple; unused members stay out of binaries that never call it.
function(bench_link_box2d target)
  if(TARGET ${target})
    target_link_libraries(${target} PRIVATE box2d)
  endif()
endfunction()


# â”€â”€ Two targets, deliberately â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
#
#   bench_selftest : the rules, and proof they are right. Zero dependencies, so
#                    it builds and runs anywhere with a C++20 compiler.
#   workbench      : the Win32 window. No third-party GUI dependencies.
#
# The split exists because the rules are the part that has to be CORRECT and the
# renderer is the part that has to be FAST, and mixing those concerns is how you
# end up unable to test either. It also means a broken graphics driver can never
# stop you verifying a rule.

option(BENCH_BUILD_APP "Build the windowed workbench (Win32). OFF = rules + self-test only." ON)

# â”€â”€ rules + verification (no dependencies) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
add_executable(bench_selftest src/self_test.cpp)
target_include_directories(bench_selftest PRIVATE src)
target_link_options(bench_selftest PRIVATE ${STATIC_CXX})

if(MSVC)
    target_compile_options(bench_selftest PRIVATE /W4 /permissive-)
else()
    target_compile_options(bench_selftest PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME rules COMMAND bench_selftest)

add_executable(bench_rendering src/rendering_test.cpp)
target_include_directories(bench_rendering PRIVATE src)
target_link_options(bench_rendering PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_rendering PRIVATE -Wall -Wextra)
endif()
add_test(NAME rendering COMMAND bench_rendering)

add_executable(bench_projects src/projects_test.cpp)
target_include_directories(bench_projects PRIVATE src)
target_link_options(bench_projects PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_projects PRIVATE -Wall -Wextra)
endif()
add_test(NAME saved_simulations COMMAND bench_projects)

# â”€â”€ the headless viewer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Renders the whole roster to a PNG with no window at all. Also means the
# rendering core is exercised on every rule, so a sim that blows up or renders
# black is caught by looking rather than by trusting.
add_executable(bench_sheet src/contact_sheet.cpp)
target_include_directories(bench_sheet PRIVATE src)
target_link_options(bench_sheet PRIVATE ${STATIC_CXX})
# The tool's freshness gate checks all headers, including UI-only headers.
# Relink after those change so a successful incremental build can run it.
file(GLOB_RECURSE BENCH_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
set_property(TARGET bench_sheet PROPERTY LINK_DEPENDS "${BENCH_HEADERS}")

# â”€â”€ the stress test â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Drives the max-speed path exactly as the app does and asserts the limits hold:
# bounded snapshot rate, bounded memory, and a frame loop that never wedges.
add_executable(bench_stress src/stress_test.cpp)
target_include_directories(bench_stress PRIVATE src)
target_link_options(bench_stress PRIVATE ${STATIC_CXX})
add_test(NAME limits COMMAND bench_stress)

# Keep the standalone subsystem checks in the normal verification path.
foreach(part biomes craft fluids light mobs planet traffic traffic_idm)
    add_executable(test_${part} test_${part}.cpp)
    target_include_directories(test_${part} PRIVATE src)
    target_link_options(test_${part} PRIVATE ${STATIC_CXX})
    if(MSVC)
        target_compile_options(test_${part} PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_${part} PRIVATE -UNDEBUG -Wall -Wextra)
    endif()
    add_test(NAME ${part} COMMAND test_${part})
endforeach()

add_executable(bench_workflows src/workflow_test.cpp)
target_include_directories(bench_workflows PRIVATE src)
target_link_options(bench_workflows PRIVATE ${STATIC_CXX})
add_test(NAME workflows COMMAND bench_workflows)

add_executable(bench_locomotion src/locomotion_test.cpp)
target_include_directories(bench_locomotion PRIVATE src)
target_link_options(bench_locomotion PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_locomotion PRIVATE -Wall -Wextra)
endif()
add_test(NAME locomotion COMMAND bench_locomotion)

add_executable(bench_sorting src/sorting_test.cpp)
target_include_directories(bench_sorting PRIVATE src)
target_link_options(bench_sorting PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_sorting PRIVATE -Wall -Wextra)
endif()
add_test(NAME sorting COMMAND bench_sorting)

add_executable(bench_collision src/collision_test.cpp)
target_include_directories(bench_collision PRIVATE src)
target_link_options(bench_collision PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_collision PRIVATE -Wall -Wextra)
endif()
add_test(NAME collision COMMAND bench_collision)

add_executable(bench_satisfy src/satisfy_test.cpp)
target_include_directories(bench_satisfy PRIVATE src)
target_link_options(bench_satisfy PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_satisfy PRIVATE -Wall -Wextra)
endif()
add_test(NAME satisfy COMMAND bench_satisfy)

add_executable(bench_sapling src/sapling_test.cpp)
target_include_directories(bench_sapling PRIVATE src)
target_link_options(bench_sapling PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_sapling PRIVATE -Wall -Wextra)
endif()
add_test(NAME sapling COMMAND bench_sapling)

add_executable(bench_fly src/fly_test.cpp)
target_include_directories(bench_fly PRIVATE src)
target_link_options(bench_fly PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_fly PRIVATE -Wall -Wextra)
endif()
add_test(NAME fly COMMAND bench_fly)

add_executable(bench_jolt_stub src/jolt_stub_test.cpp)
target_include_directories(bench_jolt_stub PRIVATE src)
if(TARGET bench_jolt)
    target_link_libraries(bench_jolt_stub PRIVATE bench_jolt)
else()
    target_link_libraries(bench_jolt_stub PRIVATE jolt_stub)
endif()
target_link_options(bench_jolt_stub PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_jolt_stub PRIVATE -Wall -Wextra)
endif()
add_test(NAME jolt_stub COMMAND bench_jolt_stub)

if(WIN32)
    add_library(example_plugin SHARED ../plugins/example_rule.cpp)
    target_include_directories(example_plugin PRIVATE src)
    target_link_options(example_plugin PRIVATE ${STATIC_CXX})
    set_target_properties(example_plugin PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
    add_executable(bench_plugin_test src/plugin_test.cpp)
    target_include_directories(bench_plugin_test PRIVATE src)
    target_link_options(bench_plugin_test PRIVATE ${STATIC_CXX})
    add_dependencies(bench_plugin_test example_plugin)
    add_test(NAME plugins COMMAND bench_plugin_test "$<TARGET_FILE_DIR:example_plugin>")
endif()

# â”€â”€ the dedicated runner â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# One simulation, the whole machine: no window, no timeline, no frame clock.
# The workbench's Dedicated run hands its current setup to this, and it stands
# alone for runs too big or too long to watch. See src/runner.hpp.
add_executable(bench_run src/runner.cpp)
target_include_directories(bench_run PRIVATE src)
target_link_options(bench_run PRIVATE ${STATIC_CXX})

add_executable(bench_rulestrings src/rulestring_test.cpp)
target_include_directories(bench_rulestrings PRIVATE src)
target_link_options(bench_rulestrings PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_rulestrings PRIVATE -Wall -Wextra)
endif()
add_test(NAME rulestrings COMMAND bench_rulestrings)

add_executable(bench_runner_test src/runner_test.cpp)
target_include_directories(bench_runner_test PRIVATE src)
target_link_options(bench_runner_test PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_run PRIVATE -Wall -Wextra)
    target_compile_options(bench_runner_test PRIVATE -Wall -Wextra)
endif()
if(WIN32)
    add_dependencies(bench_runner_test example_plugin)
    add_test(NAME runner COMMAND bench_runner_test "$<TARGET_FILE_DIR:example_plugin>")
else()
    add_test(NAME runner COMMAND bench_runner_test)
endif()
# The real executable end to end: a run that must finish, and a refusal that
# must be a refusal. Matched on the message rather than WILL_FAIL, because
# WILL_FAIL would also pass a crash.
add_test(NAME runner_cli COMMAND bench_run life --set size=256 --steps 200
         --out "${CMAKE_CURRENT_BINARY_DIR}/runner-cli")
add_test(NAME runner_cli_refuses COMMAND bench_run nosuch --steps 1)
set_tests_properties(runner_cli_refuses PROPERTIES
                     PASS_REGULAR_EXPRESSION "no simulation called 'nosuch'")

# â”€â”€ stepping off the window's thread â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# The worker's contract, and the property the window relies on: every library
# sim steps to the same world whichever thread steps it. See src/sim_worker.hpp.
add_executable(bench_worker_test src/worker_test.cpp)
target_include_directories(bench_worker_test PRIVATE src)
target_link_options(bench_worker_test PRIVATE ${STATIC_CXX})
if(NOT MSVC)
    target_compile_options(bench_worker_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME worker COMMAND bench_worker_test)

# â”€â”€ the application icon, generated by the bench itself â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# A glider, four generations into a real Conway simulation, written as a
# multi-resolution .ico with PNG payloads. No third-party asset, so no licence
# to track: the app draws things for a living, and its icon is its own output.
add_executable(bench_make_icon src/make_icon.cpp)
target_include_directories(bench_make_icon PRIVATE src)
target_link_options(bench_make_icon PRIVATE ${STATIC_CXX})

# â”€â”€ the sweep tool â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Runs one sim across a range of one or two knobs and writes a grid PNG plus a
# metric table. Turns "what does this parameter do" into a picture and numbers.
add_executable(bench_sweep src/sweep.cpp)
target_include_directories(bench_sweep PRIVATE src)
target_link_options(bench_sweep PRIVATE ${STATIC_CXX})

# â”€â”€ the workbench â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
if(NOT BENCH_BUILD_APP)
    return()
endif()

if(NOT WIN32)
    message(STATUS "Workbench is Win32-only for now; skipping. Rules, tests and the "
                   "PNG contact sheet all build everywhere.")
    return()
endif()

# Win32 and nothing else. This draws one rectangle of indexed pixels;
# StretchDIBits does that, ships with the OS, and needs no vendoring. The
# Raster seam is where a GPU backend would attach if one ever earns its place.
enable_language(RC)
add_executable(workbench WIN32 src/workbench.cpp workbench.rc)
target_include_directories(workbench PRIVATE src)
target_link_libraries(workbench PRIVATE gdi32 shell32 user32 comdlg32)
target_compile_definitions(workbench PRIVATE BENCH_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
# Dedicated runs launch bench_run from beside the workbench, so build it with it.
add_dependencies(workbench bench_run)

add_executable(bench_ui_shot src/workbench.cpp)
target_include_directories(bench_ui_shot PRIVATE src)
target_compile_definitions(bench_ui_shot PRIVATE BENCH_UI_SHOT UNICODE _UNICODE)
target_compile_definitions(bench_ui_shot PRIVATE BENCH_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
target_link_libraries(bench_ui_shot PRIVATE gdi32 shell32 user32 comdlg32)
target_link_options(bench_ui_shot PRIVATE ${STATIC_CXX})
add_dependencies(bench_ui_shot bench_run)
add_test(NAME ui_layout COMMAND bench_ui_shot --verify-ui)

if(MSVC)
    target_compile_options(workbench PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(workbench PRIVATE UNICODE _UNICODE)
else()
    target_compile_options(workbench PRIVATE -Wall -Wextra -municode)
    # Static runtime, deliberately. With MSYS2 and Git-for-Windows both on PATH,
    # a dynamically linked exe can load the WRONG libstdc++ at startup and
    # segfault constructing an ifstream - which is exactly what happened here,
    # and only in the optimised build, which made it look like a code bug.
    # A tool you double-click should not care which MinGW is first on PATH.
    # -mwindows: no console window. This is a desktop app, not a terminal tool.
    target_link_options(workbench PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
endif()

# Box2D for every target that can include the collision lab via the registry.
foreach(_bench_tgt bench_selftest bench_rendering bench_projects bench_sheet bench_stress
                   test_biomes test_craft test_fluids test_light test_mobs test_planet test_traffic test_traffic_idm
                   bench_workflows bench_locomotion bench_sorting bench_collision bench_satisfy bench_sapling bench_fly bench_jolt_stub
                   example_plugin bench_plugin_test bench_run bench_rulestrings bench_runner_test
                   bench_worker_test bench_make_icon bench_sweep workbench bench_ui_shot)
  bench_link_box2d(${_bench_tgt})
endforeach()

