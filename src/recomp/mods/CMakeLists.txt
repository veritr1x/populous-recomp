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
