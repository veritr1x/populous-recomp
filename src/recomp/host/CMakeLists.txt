# Each executable lists exactly the host sources its shell script listed, so
# which strong host_* callbacks a binary carries does not change.
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})
set(BACKENDS ${POP_ROOT}/src/backends/cpu)

# Portable host code shared by every host that boots the guest.
add_library(host_common OBJECT boot.cpp report_lock.cpp)
target_include_directories(host_common PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(host_common PUBLIC recomp_runtime)
target_compile_options(host_common PRIVATE ${POP_WARN_HOST})
pop_optimize(host_common 1)

# Compiling Objective-C++ under ARC, as every script did; a no-op for C++.
set(POP_OBJC_ARC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# The GPU interface, its test double, and (on Apple) the Metal backend.
add_subdirectory(gpu)

if(NOT APPLE)
  return()
endif()

set(FW_FOUNDATION "-framework Foundation")
set(FW_METAL "-framework Metal" "-framework MetalKit" "-framework CoreText"
             "-framework CoreVideo" "-framework CoreGraphics" "-framework QuartzCore")
set(FW_AUDIO "-framework AVFoundation" "-framework AudioToolbox")

# pop_host(<target> [GEN] [UI_LAYER] [OPT <n>] SOURCES ... LIBS ... FRAMEWORKS ...):
# a host or a test binary over the runtime, shims, mods and Lua.
function(pop_host target)
  cmake_parse_arguments(ARG "GEN;UI_LAYER" "OPT" "SOURCES;FRAMEWORKS;LIBS" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} ${ARG_FRAMEWORKS})
  target_include_directories(${target} PRIVATE ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime
                                               ${POP_ROOT}/src/recomp/host ${POP_ROOT}/third_party/lua)
  target_compile_options(${target} PRIVATE ${POP_WARN_HOST} ${POP_OBJC_ARC})
  if(ARG_UI_LAYER)
    target_compile_definitions(${target} PRIVATE POPM_PRESENT_HAS_UI_LAYER=1)
  endif()
  if(NOT ARG_OPT)
    set(ARG_OPT 1)
  endif()
  pop_optimize(${target} ${ARG_OPT})
  if(ARG_GEN)
    pop_link_gen(${target})
  endif()
endfunction()

set(POP_CORE_LIBS recomp_platform recomp_runtime recomp_snapshot recomp_dx recomp_mods lua host_common gpu_metal)
set(POP_PRESENT_SOURCES ${HOST}/present_pixels.cpp ${HOST}/present_thread.cpp ${HOST}/compositor.cpp
                        ${HOST}/ui_layer.cpp ${HOST}/performance_overlay.cpp ${HOST}/d3d_render.cpp
                        ${BACKENDS}/indexed_frame.cpp)

if(TARGET recomp_gen)
  pop_host(pop_headless GEN
    SOURCES ${HOST}/headless_main.cpp ${HOST}/page_overlay.cpp
            ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_AUDIO})
  add_dependencies(pop_headless core_mods)

  pop_host(pop_smoke GEN UI_LAYER
    SOURCES ${HOST}/smoke_main.mm ${HOST}/script.cpp ${HOST}/input.mm ${HOST}/input_gate.cpp
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/audio_math.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  add_dependencies(pop_smoke core_mods smoke_probe)

  pop_host(PopRecomp GEN UI_LAYER OPT 2
    SOURCES ${HOST}/sdl/main.cpp ${HOST}/sdl/keymap.cpp ${HOST}/present.cpp ${HOST}/page_overlay.cpp
            ${POP_PRESENT_SOURCES}
            ${HOST}/input.mm ${HOST}/input_gate.cpp
            ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp ${HOST}/midi.mm
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_METAL} ${FW_AUDIO})
  pop_link_sdl(PopRecomp)
  pop_mac_bundle(PopRecomp)

  # Real loader, hooks and DirectDraw seal/service with fake GPU acks.
  pop_host(present_events_tests GEN UI_LAYER
    SOURCES ${POP_ROOT}/src/recomp/mods/tests/present_events_tests.mm
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.mm ${HOST}/input_gate.cpp
    LIBS recomp_platform recomp_runtime_testing recomp_snapshot recomp_dx recomp_mods_testing lua gpu_metal
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  add_test(NAME present_events_tests COMMAND present_events_tests ${POP_ROOT} WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(present_events_tests PROPERTIES LABELS mods)
endif()

# Headless host tests: offscreen Metal and the real audio engine rendered
# offline, no window. Labelled `device`: a hosted CI runner has no audio
# device and its offline render does not reach the levels the suite asserts.
# report_lock.cpp is a source here rather than host_common, because the script
# never linked boot.cpp into this suite.
pop_host(host_tests UI_LAYER
  SOURCES ${HOST}/tests/host_tests.mm ${HOST}/tests/test_frame_builder.cpp
          ${POP_ROOT}/src/recomp/runtime/tests/stub_recomp_call.cpp
          ${HOST}/report_lock.cpp ${HOST}/script.cpp ${HOST}/midi.mm ${HOST}/present.cpp
          ${POP_ROOT}/src/recomp/mods/font6x8.cpp
          ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.mm ${HOST}/input_gate.cpp
          ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp
          ${POP_ROOT}/src/recomp/dx/com.cpp ${POP_ROOT}/src/recomp/dx/host_api.cpp
          ${POP_ROOT}/src/recomp/dx/display_stubs.cpp ${POP_ROOT}/src/recomp/dx/dinput.cpp
  LIBS recomp_platform recomp_runtime gpu_metal gpu_fake
  FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL} ${FW_AUDIO})
pop_test_binary(host_tests)
add_test(NAME host_tests COMMAND host_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_tests PROPERTIES LABELS device)

add_executable(compositor_tests ${HOST}/compositor.cpp ${HOST}/tests/compositor_tests.cpp)
target_include_directories(compositor_tests PRIVATE ${POP_ROOT} ${HOST})
target_compile_options(compositor_tests PRIVATE ${POP_WARN_WERROR})
target_link_libraries(compositor_tests PRIVATE gpu_metal)
pop_optimize(compositor_tests 1)
pop_test_binary(compositor_tests)
add_test(NAME compositor_tests COMMAND compositor_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(compositor_tests PROPERTIES LABELS gpu)

add_executable(ui_layer_tests ${HOST}/ui_layer.cpp ${HOST}/tests/test_frame_builder.cpp ${HOST}/tests/ui_layer_tests.cpp)
target_compile_options(ui_layer_tests PRIVATE ${POP_WARN_WERROR})
pop_optimize(ui_layer_tests 1)
pop_test_binary(ui_layer_tests)
add_test(NAME ui_layer_tests COMMAND ui_layer_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(ui_layer_tests PROPERTIES LABELS nogame)
