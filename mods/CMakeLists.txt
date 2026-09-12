# The plugins the tree ships: bundled core mods through the installer, the
# examples beside their manifests, and the smoke probe.
add_custom_target(core_mods
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/build_core.py
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
set_tests_properties(test_build_core PROPERTIES LABELS nogame ENVIRONMENT "POP_CC=${CMAKE_C_COMPILER}")
