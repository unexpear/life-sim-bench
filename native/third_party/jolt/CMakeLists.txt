# Jolt 5.6.0 optional scaffolding + optional full link of a fetched pin.
# See PIN.md / native/scripts/fetch_jolt_pin.ps1.
# Default: INTERFACE stub + analytic SphereWorld headers (no upstream compile).
# BENCH_WITH_JOLT=ON: add_subdirectory JoltPhysics/Build (library only when not
# the CMake root) and define BENCH_HAS_JOLT for dependents.
cmake_minimum_required(VERSION 3.22)

add_library(jolt_stub INTERFACE)
target_include_directories(jolt_stub INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

option(BENCH_WITH_JOLT "Link a local JoltPhysics checkout (experimental)" OFF)
set(BENCH_JOLT_PIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JoltPhysics" CACHE PATH
    "Fetched JoltPhysics tree (gitignored); from fetch_jolt_pin.ps1")

if(BENCH_WITH_JOLT)
  if(EXISTS "${BENCH_JOLT_PIN_DIR}/Build/CMakeLists.txt")
    # Keep the embed lean: disable install noise; extras are skipped by upstream
    # when Build/ is not the CMake source root.
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BENCH_JOLT_PIN_DIR}/Build" "${CMAKE_BINARY_DIR}/third_party/jolt_physics" EXCLUDE_FROM_ALL)
    if(TARGET Jolt)
      add_library(bench_jolt INTERFACE)
      target_link_libraries(bench_jolt INTERFACE Jolt jolt_stub)
      target_compile_definitions(bench_jolt INTERFACE BENCH_HAS_JOLT=1)
      message(STATUS "BENCH_WITH_JOLT: linked upstream Jolt from ${BENCH_JOLT_PIN_DIR}")
    else()
      message(WARNING "BENCH_WITH_JOLT: Jolt target missing after add_subdirectory")
    endif()
  else()
    message(WARNING "BENCH_WITH_JOLT=ON but JoltPhysics checkout missing; run fetch_jolt_pin.ps1")
  endif()
endif()
