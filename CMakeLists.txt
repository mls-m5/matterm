
cmake_minimum_required(VERSION 3.23)
project(matterm LANGUAGES CXX)

find_package(SDL2 2.0.18 REQUIRED)
find_package(SDL2_ttf 2.0.18 REQUIRED)

add_library(cellbuffer src/cellbuffer.cpp)
target_compile_features(cellbuffer PUBLIC cxx_std_17)
target_include_directories(cellbuffer PUBLIC src)

add_library(terminalmemory src/terminalmemory.cpp src/scrollviewport.cpp)
target_compile_features(terminalmemory PUBLIC cxx_std_17)
target_include_directories(terminalmemory PUBLIC src)

add_library(glyphatlas src/glyphatlas.cpp)
target_compile_features(glyphatlas PUBLIC cxx_std_17)
target_include_directories(glyphatlas PUBLIC src)
target_link_libraries(glyphatlas PUBLIC SDL2::SDL2 SDL2_ttf::SDL2_ttf)

add_library(sdlbackend src/sdlbackend.cpp)
target_link_libraries(sdlbackend PUBLIC glyphatlas)

# Shared rendering API for demos, future terminal emulation, and other clients.
add_library(matterm-renderer INTERFACE)
add_library(matterm::renderer ALIAS matterm-renderer)
target_link_libraries(matterm-renderer INTERFACE cellbuffer terminalmemory sdlbackend)

include(CTest)
option(MATTERM_BUILD_DEMO "Build the standalone matterm rendering demo" ON)

# Demo helpers are deliberately separate from the reusable renderer.
if(MATTERM_BUILD_DEMO OR BUILD_TESTING)
    add_library(democontent demo/democontent.cpp)
    target_include_directories(democontent PUBLIC demo)
    target_link_libraries(democontent PUBLIC cellbuffer terminalmemory)
endif()

if(MATTERM_BUILD_DEMO)
    add_executable(matterm-demo demo/main.cpp)
    target_link_libraries(matterm-demo PRIVATE matterm::renderer democontent)
endif()
add_subdirectory(lib)
add_subdirectory(test)

