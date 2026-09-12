# The translated game. A developer's fresh regeneration in build/recomp/gen
# wins; otherwise the tracked translation in translation/ (the one supported
# GOG build) is what every host links.
if(EXISTS ${POP_OUT}/gen/table.c)
  set(POP_GEN_DIR ${POP_OUT}/gen)
else()
  set(POP_GEN_DIR ${POP_ROOT}/translation)
endif()
set(POP_HAVE_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: PopRecomp, pop_headless, pop_smoke, pop_fixture, "
                 "mods_tests, present_events_tests and profile_tests are not defined")
endif()

if(POP_HAVE_GEN)
  file(GLOB POP_GEN_SOURCES CONFIGURE_DEPENDS ${POP_GEN_DIR}/chunk_*.c ${POP_GEN_DIR}/table.c)
  add_library(recomp_gen STATIC ${POP_GEN_SOURCES})
  set_target_properties(recomp_gen PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${POP_OUT} OUTPUT_NAME recomp_gen)
  # -I<gen> for x86.h beside the sources, -I<root> for the canonical copy,
  # -I<runtime> for intrinsics.h: the same three the shell script passed.
  target_include_directories(recomp_gen PRIVATE ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime)
  target_include_directories(recomp_gen INTERFACE ${POP_GEN_DIR})
  target_compile_options(recomp_gen PRIVATE ${POP_WARN_GEN})
  pop_optimize(recomp_gen 2)
endif()

# The portable spelling of -Wl,-force_load: every generated object is kept
# whether or not anything references it, because the dispatch table is
# reached by address.
function(pop_link_gen target)
  target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>")
  target_include_directories(${target} PRIVATE ${POP_GEN_DIR})
endfunction()
