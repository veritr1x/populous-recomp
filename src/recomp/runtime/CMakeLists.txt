# The guest runtime. Object libraries, so every source is linked into every
# consumer exactly as the shell scripts linked every object.
set(POP_RUNTIME_SOURCES
  memory.cpp loader.cpp imports.cpp cpu.cpp profile.cpp kernel32.cpp
  mods_seam.cpp user32.cpp misc.cpp)

add_library(recomp_runtime OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime PUBLIC recomp_platform)
target_compile_options(recomp_runtime PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime 2)

# POPM_TESTING compiles the test-only entry points (loader reset, owner
# counter seam, scheduler latch reset). None exists in a real build.
add_library(recomp_runtime_testing OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime_testing PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime_testing PUBLIC recomp_platform)
target_compile_definitions(recomp_runtime_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_runtime_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime_testing 1)

# Linked by the hosts, the fixture and the mod tests, not by the unit tests.
add_library(recomp_snapshot OBJECT snapshot.cpp)
target_link_libraries(recomp_snapshot PUBLIC recomp_runtime)
target_compile_options(recomp_snapshot PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_snapshot 2)

if(NOT WIN32)
  # The suite forks and waits; it is POSIX-only until sub-project 3.
  add_executable(runtime_tests tests/runtime_tests.cpp tests/stub_recomp_call.cpp)
  target_link_libraries(runtime_tests PRIVATE recomp_platform recomp_runtime)
  target_compile_options(runtime_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(runtime_tests 1)
  pop_test_binary(runtime_tests)
  add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(runtime_tests PROPERTIES LABELS game)
endif()

if(TARGET recomp_gen)
  # The headless parity fixture: null host, real generated code, core mods installed.
  add_executable(pop_fixture fixture.cpp)
  target_link_libraries(pop_fixture PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua)
  target_compile_options(pop_fixture PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture 1)
  pop_link_gen(pop_fixture)
  add_dependencies(pop_fixture core_mods)

  add_executable(profile_tests tests/profile_tests.cpp)
  target_link_libraries(profile_tests PRIVATE recomp_platform recomp_runtime recomp_dx_null)
  target_compile_options(profile_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(profile_tests 1)
  pop_link_gen(profile_tests)
  pop_test_binary(profile_tests)
  add_test(NAME profile_tests_disabled COMMAND profile_tests disabled WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(profile_tests_disabled PROPERTIES LABELS game ENVIRONMENT POPM_PROFILE=0)
  add_test(NAME profile_tests_enabled COMMAND profile_tests enabled WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(profile_tests_enabled PROPERTIES LABELS game ENVIRONMENT POPM_PROFILE=1)
endif()

if(POP_TRACE_DIR)
  # Caller-supplied trace wrappers over a snapshot of gen/: the override header
  # is force-included ahead of funcs.h so every FN_<addr> macro is already set.
  file(GLOB POP_TRACE_SOURCES CONFIGURE_DEPENDS ${POP_TRACE_DIR}/gen/chunk_*.c ${POP_TRACE_DIR}/gen/table.c)
  add_library(recomp_gen_trace OBJECT ${POP_TRACE_SOURCES} ${POP_TRACE_DIR}/trace_wrappers.c)
  target_include_directories(recomp_gen_trace PRIVATE ${POP_TRACE_DIR} ${POP_TRACE_DIR}/gen ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime)
  target_compile_options(recomp_gen_trace PRIVATE ${POP_WARN_GEN} -include ${POP_TRACE_DIR}/trace_override.h)
  pop_optimize(recomp_gen_trace 1)
  add_executable(pop_fixture_trace fixture.cpp)
  target_link_libraries(pop_fixture_trace PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua recomp_gen_trace)
  target_include_directories(pop_fixture_trace PRIVATE ${POP_TRACE_DIR}/gen)
  target_compile_options(pop_fixture_trace PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture_trace 1)
endif()
