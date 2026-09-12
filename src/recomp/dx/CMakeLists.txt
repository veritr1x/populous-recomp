set(POP_DX_SOURCES
  com.cpp dx.cpp host_api.cpp display_stubs.cpp ddraw.cpp d3d.cpp
  dsound.cpp dinput.cpp qmixer.cpp weanetr.cpp)

add_library(recomp_dx OBJECT ${POP_DX_SOURCES})
target_link_libraries(recomp_dx PUBLIC recomp_runtime)
target_compile_options(recomp_dx PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx 2)

# The same shims with every host callback a strong no-op: a binary linking
# these can neither draw nor play sound. The parity fixture and the profile
# tests use it.
add_library(recomp_dx_null OBJECT ${POP_DX_SOURCES})
target_link_libraries(recomp_dx_null PUBLIC recomp_runtime)
target_compile_definitions(recomp_dx_null PUBLIC RECOMP_NULL_HOST)
target_compile_options(recomp_dx_null PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx_null 1)

# host_api.h is a C header too. The C11 consumer is an object the DX tests
# link; the header is also parsed as C++17 on its own.
add_library(host_api_header_test OBJECT tests/host_api_header_test.c)
target_include_directories(host_api_header_test PRIVATE ${POP_ROOT})
target_compile_options(host_api_header_test PRIVATE -Wall -Wextra)
pop_optimize(host_api_header_test 1)
add_test(NAME host_api_header_check
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter
          -I${POP_ROOT} -fsyntax-only -x c++ ${CMAKE_CURRENT_SOURCE_DIR}/host_api.h)
set_tests_properties(host_api_header_check PROPERTIES LABELS nogame)

add_executable(dx_tests tests/dx_tests.cpp
  ${POP_ROOT}/src/recomp/runtime/tests/stub_recomp_call.cpp
  $<TARGET_OBJECTS:host_api_header_test>)
target_link_libraries(dx_tests PRIVATE recomp_dx recomp_runtime)
target_compile_options(dx_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(dx_tests 1)
pop_test_binary(dx_tests)
add_test(NAME dx_tests COMMAND dx_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(dx_tests PROPERTIES LABELS nogame)
