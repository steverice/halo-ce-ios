cmake_minimum_required(VERSION 3.24)
project(RendererOptionsProbe NONE)
include("${CMAKE_CURRENT_LIST_DIR}/../../../RendererBackendOptions.cmake")
message(STATUS "renderer probe SDL_OPENGLES=${SDL_OPENGLES}")
