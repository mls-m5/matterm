
cmake_minimum_required(VERSION 3.23)
project(2026-terminal-renderer LANGUAGES CXX)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(2026-terminal-renderer src/main.cpp)
target_compile_features(2026-terminal-renderer PRIVATE cxx_std_17)
target_link_libraries(2026-terminal-renderer PRIVATE SDL2::SDL2 OpenGL::GL)

include(CTest)
add_subdirectory(lib)
add_subdirectory(test)

