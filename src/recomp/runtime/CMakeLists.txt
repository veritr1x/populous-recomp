# The guest runtime. Object libraries, so every source is linked into every
# consumer exactly as the shell scripts linked every object.
set(POP_RUNTIME_SOURCES
  memory.cpp loader.cpp imports.cpp cpu.cpp profile.cpp kernel32.cpp
  mods_seam.cpp user32.cpp misc.cpp)

add_library(recomp_runtime OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(recomp_runtime PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime 2)

# POPM_TESTING compiles the test-only entry points (loader reset, owner
# counter seam, scheduler latch reset). None exists in a real build.
add_library(recomp_runtime_testing OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime_testing PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
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
  target_link_libraries(runtime_tests PRIVATE recomp_runtime)
  target_compile_options(runtime_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(runtime_tests 1)
  pop_test_binary(runtime_tests)
  add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(runtime_tests PROPERTIES LABELS game)
endif()
