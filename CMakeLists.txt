cmake_minimum_required(VERSION 3.24)
project(PopulousRecomp LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization is a per-target decision (cmake/Warnings.cmake). The
# configuration only decides whether -O0 is forced.
foreach(lang C CXX OBJCXX)
  set(CMAKE_${lang}_FLAGS_RELEASE "-g")
  set(CMAKE_${lang}_FLAGS_DEBUG "-g -O0")
endforeach()

set(POP_ROOT ${CMAKE_SOURCE_DIR})
set(POP_OUT ${POP_ROOT}/build/recomp)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${POP_OUT})

set(POP_TRANSLATE AUTO CACHE STRING "AUTO, ON or OFF: define targets that need build/recomp/gen")
set_property(CACHE POP_TRANSLATE PROPERTY STRINGS AUTO ON OFF)
set(POP_TRACE_DIR "" CACHE PATH "Snapshot directory for pop_fixture_trace")
set(POP_RECOMP_APP_NAME PopRecomp CACHE STRING "macOS bundle name")
if(NOT POP_RECOMP_APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid POP_RECOMP_APP_NAME: ${POP_RECOMP_APP_NAME}")
endif()

# tools/build.py passes its own interpreter; a bare configure finds one.
if(NOT Python3_EXECUTABLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

enable_testing()
add_custom_target(check_binaries)

include(cmake/Warnings.cmake)
include(cmake/Plugins.cmake)
include(cmake/Translate.cmake)
include(cmake/MacBundle.cmake)
add_subdirectory(src/recomp/runtime)
add_subdirectory(src/recomp/dx)
add_subdirectory(src/recomp/mods)
add_subdirectory(mods)
add_subdirectory(src/recomp/host)
