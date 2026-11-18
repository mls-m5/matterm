
cmake_minimum_required(VERSION 3.23)
project(2026-terminal-renderer LANGUAGES CXX)

find_package(SDL2 2.0.18 REQUIRED)
find_package(SDL2_ttf 2.0.18 REQUIRED)

add_library(cellbuffer src/cellbuffer.cpp)
target_compile_features(cellbuffer PUBLIC cxx_std_17)
target_include_directories(cellbuffer PUBLIC src)

add_library(glyphatlas src/glyphatlas.cpp)
target_compile_features(glyphatlas PUBLIC cxx_std_17)
target_include_directories(glyphatlas PUBLIC src)
target_link_libraries(glyphatlas PUBLIC SDL2::SDL2 SDL2_ttf::SDL2_ttf)

add_library(sdlbackend src/sdlbackend.cpp)
target_link_libraries(sdlbackend PUBLIC glyphatlas)

add_executable(2026-terminal-renderer src/main.cpp)
target_compile_features(2026-terminal-renderer PRIVATE cxx_std_17)
target_link_libraries(2026-terminal-renderer PRIVATE cellbuffer sdlbackend)

include(CTest)
add_subdirectory(lib)
add_subdirectory(test)

