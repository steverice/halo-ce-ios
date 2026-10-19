# Keep a reused iOS or Catalyst build directory consistent when its renderer selection changes.
if(HALO_RENDERER STREQUAL "METAL" OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(SDL_OPENGLES ON CACHE BOOL "" FORCE)
endif()
