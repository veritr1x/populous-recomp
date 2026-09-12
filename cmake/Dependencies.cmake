# Third-party code that is fetched rather than vendored. Pinned to a tag, never
# a branch, so a configure today and a configure next year build the same bytes.
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Subsystems the host does not use stay out of the binary.
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.16
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# pop_link_sdl(<target>): link SDL3 statically and give the target its headers.
function(pop_link_sdl target)
  target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
endfunction()
