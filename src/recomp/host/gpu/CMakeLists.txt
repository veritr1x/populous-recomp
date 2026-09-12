# The GPU interface, its test double, and the platform backend.
add_library(gpu_fake OBJECT fake/fake_device.cpp)
target_include_directories(gpu_fake PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gpu_fake PRIVATE ${POP_WARN_STRICT})
pop_optimize(gpu_fake 1)

add_executable(gpu_fake_tests fake/tests/gpu_fake_tests.cpp)
target_link_libraries(gpu_fake_tests PRIVATE gpu_fake)
target_compile_options(gpu_fake_tests PRIVATE ${POP_WARN_STRICT})
pop_optimize(gpu_fake_tests 1)
pop_test_binary(gpu_fake_tests)
add_test(NAME gpu_fake_tests COMMAND gpu_fake_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(gpu_fake_tests PROPERTIES LABELS nogame)

# The platform backend and the factory. Without Metal the factory returns
# nullptr until sub-project 3 adds Vulkan.
if(APPLE)
  add_library(gpu_metal OBJECT metal/metal_device.mm metal/metal_surface.mm gpu_factory.cpp)
  target_compile_options(gpu_metal PRIVATE ${POP_WARN_HOST} $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(gpu_metal PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                                         "-framework CoreGraphics" "-framework Foundation")
  pop_optimize(gpu_metal 2)

  add_executable(gpu_metal_tests metal/tests/gpu_metal_tests.cpp)
  target_link_libraries(gpu_metal_tests PRIVATE gpu_metal)
  target_compile_options(gpu_metal_tests PRIVATE ${POP_WARN_STRICT})
  pop_optimize(gpu_metal_tests 1)
  pop_test_binary(gpu_metal_tests)
  add_test(NAME gpu_metal_tests COMMAND gpu_metal_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(gpu_metal_tests PROPERTIES LABELS gpu)
else()
  add_library(gpu_metal OBJECT gpu_factory.cpp)
  target_compile_options(gpu_metal PRIVATE ${POP_WARN_HOST})
  pop_optimize(gpu_metal 1)
endif()
target_include_directories(gpu_metal PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
