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
if(APPLE)
  set(POP_OBJC_ARC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
  set(POP_OBJC_ARC "")
endif()

# The GPU interface, its test double, the Vulkan backend and (on Apple) Metal.
add_subdirectory(gpu)

# The platform boundary: no Objective-C++ and no Apple framework outside gpu/metal/.
add_test(NAME host_boundary_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_host_boundary.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_boundary_check PROPERTIES LABELS nogame)

# The committed SPIR-V must match the GLSL sources; skips where glslc is absent.
add_test(NAME shader_drift_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_shaders.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(shader_drift_check PROPERTIES LABELS nogame)

# The tracked translation names the loader's pinned exe and is complete.
add_test(NAME translation_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_translation.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(translation_check PROPERTIES LABELS nogame)

# Portable pieces built everywhere. compositor_tests needs a real device for
# its pixel half (label gpu); ui_layer_tests needs nothing.
add_executable(compositor_tests ${HOST}/compositor.cpp ${HOST}/tests/compositor_tests.cpp)
target_include_directories(compositor_tests PRIVATE ${POP_ROOT} ${HOST})
target_compile_options(compositor_tests PRIVATE ${POP_WARN_WERROR})
target_link_libraries(compositor_tests PRIVATE gpu_backend)
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

# The hosts and the full host suite build on every platform over gpu_backend.
if(APPLE)
  set(FW_FOUNDATION "-framework Foundation")
  set(FW_METAL "-framework Metal" "-framework MetalKit" "-framework CoreText"
               "-framework CoreVideo" "-framework CoreGraphics" "-framework QuartzCore")
else()
  set(FW_FOUNDATION "")
  set(FW_METAL "")
endif()

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

# The mixer, its output device and the synth: portable, over SDL audio.
set(POP_AUDIO_SOURCES ${HOST}/audio/mixer.cpp ${HOST}/audio/sdl_sink.cpp ${HOST}/audio/midi_synth.cpp
                      ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp)
set(POP_CORE_LIBS recomp_platform recomp_runtime recomp_snapshot recomp_dx recomp_mods lua host_common gpu_backend)
set(POP_PRESENT_SOURCES ${HOST}/present_pixels.cpp ${HOST}/present_thread.cpp ${HOST}/compositor.cpp
                        ${HOST}/ui_layer.cpp ${HOST}/performance_overlay.cpp ${HOST}/d3d_render.cpp
                        ${BACKENDS}/indexed_frame.cpp)

if(TARGET recomp_gen)
  pop_host(pop_headless GEN
    SOURCES ${HOST}/headless_main.cpp ${HOST}/page_overlay.cpp ${POP_AUDIO_SOURCES}
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION})
  pop_link_sdl(pop_headless)
  add_dependencies(pop_headless core_mods)

  pop_host(pop_smoke GEN UI_LAYER
    SOURCES ${HOST}/smoke_main.cpp ${HOST}/script.cpp ${HOST}/input.cpp ${HOST}/input_gate.cpp
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/audio_math.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  add_dependencies(pop_smoke core_mods smoke_probe)

  pop_host(PopRecomp GEN UI_LAYER OPT 2
    SOURCES ${HOST}/sdl/main.cpp ${HOST}/sdl/keymap.cpp ${HOST}/present.cpp ${HOST}/page_overlay.cpp
            ${POP_PRESENT_SOURCES}
            ${HOST}/input.cpp ${HOST}/input_gate.cpp ${POP_AUDIO_SOURCES}
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_METAL})
  pop_link_sdl(PopRecomp)
  if(APPLE)
    pop_mac_bundle(PopRecomp)
  endif()

  if(APPLE) # Objective-C++ source
    # Real loader, hooks and DirectDraw seal/service with fake GPU acks.
    pop_host(present_events_tests GEN UI_LAYER
      SOURCES ${POP_ROOT}/src/recomp/mods/tests/present_events_tests.mm
              ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.cpp ${HOST}/input_gate.cpp
      LIBS recomp_platform recomp_runtime_testing recomp_snapshot recomp_dx recomp_mods_testing lua gpu_backend
      FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
    add_test(NAME present_events_tests COMMAND present_events_tests ${POP_ROOT} WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(present_events_tests PROPERTIES LABELS mods)
  endif()
endif()

# Host tests: offscreen GPU work and the mixer rendered offline, no window and
# no audio device. Labelled `gpu`: the renderer tests need a device.
# report_lock.cpp is a source here rather than host_common, because the script
# never linked boot.cpp into this suite.
pop_host(host_tests UI_LAYER
  SOURCES ${HOST}/tests/host_tests.cpp ${HOST}/tests/test_frame_builder.cpp
          ${POP_ROOT}/src/recomp/runtime/tests/stub_recomp_call.cpp
          ${HOST}/report_lock.cpp ${HOST}/script.cpp ${HOST}/present.cpp
          ${POP_ROOT}/src/recomp/mods/font6x8.cpp ${POP_ROOT}/src/recomp/mods/layout.cpp
          ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.cpp ${HOST}/input_gate.cpp
          ${POP_AUDIO_SOURCES}
          ${POP_ROOT}/src/recomp/dx/com.cpp ${POP_ROOT}/src/recomp/dx/host_api.cpp
          ${POP_ROOT}/src/recomp/dx/display_stubs.cpp ${POP_ROOT}/src/recomp/dx/dinput.cpp
  LIBS recomp_platform recomp_runtime gpu_backend gpu_fake
  FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
pop_link_sdl(host_tests)
pop_test_binary(host_tests)
add_test(NAME host_tests COMMAND host_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_tests PROPERTIES LABELS gpu)

