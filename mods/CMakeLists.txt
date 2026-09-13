# The plugins the tree ships: bundled core mods through the installer, the
# examples beside their manifests, and the smoke probe.
add_custom_target(core_mods
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/build_core.py
          --source ${CMAKE_CURRENT_SOURCE_DIR}/core
          --dest ${POP_OUT}/mods/core --cc ${CMAKE_C_COMPILER}
  WORKING_DIRECTORY ${POP_ROOT}
  COMMENT "Installing core mods into build/recomp/mods/core"
  VERBATIM)

file(GLOB POP_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*/*.c)
add_custom_target(example_mods)
foreach(src ${POP_EXAMPLE_SOURCES})
  get_filename_component(stem ${src} NAME_WE)
  get_filename_component(dir ${src} DIRECTORY)
  pop_add_plugin(example_${stem} SOURCE ${src} OUTPUT_DIR ${dir} OUTPUT_NAME ${stem}
    WARNINGS ${POP_WARN_STRICT} OPTIONS -g)
  add_dependencies(example_mods example_${stem})
endforeach()

pop_add_plugin(smoke_probe SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/smoke/probe.c
  OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/smoke OUTPUT_NAME probe OPTIONS -g)

add_custom_target(plugins DEPENDS core_mods example_mods smoke_probe mod_fixtures)

add_test(NAME test_build_core
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_build_core.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(test_build_core PROPERTIES LABELS nogame ENVIRONMENT "RECOMP_CC=${CMAKE_C_COMPILER}")

# The roots resolver, run from a relocated app-shaped directory by roots_tests.py.
# The test asserts the player layout under build/; a stub build under
# build/stub/ is not a player layout, so it is not registered there.
if(NOT POP_TRANSLATE STREQUAL "STUB" AND NOT IOS)
add_executable(roots_probe ${POP_ROOT}/mods/roots.cpp ${POP_ROOT}/runtime/layout.cpp ${CMAKE_CURRENT_SOURCE_DIR}/core/tests/roots_probe.cpp)
target_include_directories(roots_probe PRIVATE ${POP_ROOT}/mods)
target_link_libraries(roots_probe PRIVATE recomp_platform)
pop_optimize(roots_probe 1)
pop_test_binary(roots_probe)
add_test(NAME roots_tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core/tests/roots_tests.py $<TARGET_FILE:roots_probe>
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(roots_tests PROPERTIES LABELS nogame)
endif()
