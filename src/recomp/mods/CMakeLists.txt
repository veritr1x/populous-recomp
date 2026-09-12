# Vendored Lua 5.4: the library only, lua.c and luac.c are not vendored.
file(GLOB POP_LUA_SOURCES CONFIGURE_DEPENDS ${POP_ROOT}/third_party/lua/*.c)
add_library(lua STATIC ${POP_LUA_SOURCES})
set_target_properties(lua PROPERTIES
  C_STANDARD 99 C_EXTENSIONS OFF
  ARCHIVE_OUTPUT_DIRECTORY ${POP_OUT} OUTPUT_NAME lua)
target_include_directories(lua PUBLIC ${POP_ROOT}/third_party/lua)
target_compile_options(lua PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(lua PRIVATE
  $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
  $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX>)
pop_optimize(lua 2)

# The mod foundation. Globbed as the scripts globbed, so a new module is
# picked up by the next configure.
file(GLOB POP_MODS_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/lua/*.cpp)
# The capture seam includes the page-tracking harness from src/recomp/native,
# which is mprotect and signal handlers. Windows gets the stub that says so.
list(REMOVE_ITEM POP_MODS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/capture_seam_win32.cpp)
if(WIN32)
  list(REMOVE_ITEM POP_MODS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/capture_seam.cpp)
  list(APPEND POP_MODS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/capture_seam_win32.cpp)
endif()

add_library(recomp_mods OBJECT ${POP_MODS_SOURCES})
target_include_directories(recomp_mods PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(recomp_mods PUBLIC lua recomp_runtime)
target_compile_options(recomp_mods PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_mods 2)

add_library(recomp_mods_testing OBJECT ${POP_MODS_SOURCES})
target_include_directories(recomp_mods_testing PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(recomp_mods_testing PUBLIC lua recomp_runtime_testing)
target_compile_definitions(recomp_mods_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_mods_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_mods_testing 1)

# Plugin fixtures the loader tests load. Every .c becomes a module of the same
# name; old_cpu is built with the older header AHEAD of the real one so its
# sizeof(pop_cpu_v1) really is the older value.
file(GLOB POP_FIXTURE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.c)
add_custom_target(mod_fixtures)
foreach(src ${POP_FIXTURE_SOURCES})
  get_filename_component(stem ${src} NAME_WE)
  set(first "")
  if(stem STREQUAL "old_cpu")
    set(first INCLUDE_FIRST ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_header)
  endif()
  pop_add_plugin(fixture_${stem} SOURCE ${src} OUTPUT_DIR ${POP_OUT}/mods-fixtures
    OUTPUT_NAME ${stem} ${first} OPTIONS -g)
  add_dependencies(mod_fixtures fixture_${stem})
endforeach()

# The roots resolver, run from a relocated app-shaped directory by roots_tests.py.
add_executable(roots_probe roots.cpp ${POP_ROOT}/mods/core/tests/roots_probe.cpp)
target_include_directories(roots_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(roots_probe PRIVATE recomp_platform)
pop_optimize(roots_probe 1)
pop_test_binary(roots_probe)
add_test(NAME roots_tests
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/mods/core/tests/roots_tests.py $<TARGET_FILE:roots_probe>
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(roots_tests PROPERTIES LABELS nogame)

if(TARGET recomp_gen)
  add_library(mods_api_header_test OBJECT tests/api_header_test.c)
  target_compile_definitions(mods_api_header_test PRIVATE POPM_TESTING=1)
  target_include_directories(mods_api_header_test PRIVATE ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime ${POP_GEN_DIR})
  target_compile_options(mods_api_header_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
  pop_optimize(mods_api_header_test 1)

  file(GLOB POP_MODS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(mods_tests ${POP_MODS_TEST_SOURCES} $<TARGET_OBJECTS:mods_api_header_test>)
  target_link_libraries(mods_tests PRIVATE recomp_platform recomp_runtime_testing recomp_snapshot recomp_mods_testing lua)
  target_compile_options(mods_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(mods_tests 1)
  pop_link_gen(mods_tests)
  add_dependencies(mods_tests mod_fixtures core_mods)
  add_test(NAME mods_tests COMMAND mods_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(mods_tests PROPERTIES LABELS mods)
endif()
