# Jolt 5.6.0 optional scaffolding. Full engine source is fetched separately
# (see PIN.md / native/scripts/fetch_jolt_pin.ps1). This stub always defines
# an INTERFACE target so dependents can compile body-interface headers.
cmake_minimum_required(VERSION 3.22)

add_library(jolt_stub INTERFACE)
target_include_directories(jolt_stub INTERFACE
  /include
  /../../src)

# When the pin is fetched under third_party/jolt/JoltPhysics, callers may
# set BENCH_WITH_JOLT=ON and add_subdirectory that tree themselves. We do
# not auto-add it here: an incomplete fetch must not break the default build.
option(BENCH_WITH_JOLT "Link a local JoltPhysics checkout (experimental)" OFF)
if(BENCH_WITH_JOLT)
  if(EXISTS "/JoltPhysics/Build/CMakeLists.txt")
    message(STATUS "BENCH_WITH_JOLT: expecting manual target wiring; stub only for now")
  else()
    message(WARNING "BENCH_WITH_JOLT=ON but JoltPhysics checkout missing; run fetch_jolt_pin.ps1")
  endif()
endif()
