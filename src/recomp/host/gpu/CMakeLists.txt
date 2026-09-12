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
