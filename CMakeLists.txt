
cmake_minimum_required(VERSION 3.23)
project(2026-terminal-renderer)

enable_testing()
add_subdirectory(lib)

add_executable(
    2026-terminal-renderer
    src/main.cpp
    )

target_compile_features(
    2026-terminal-renderer
    PRIVATE
    cxx_std_17
    )

find_package(Threads)
target_link_libraries(
    2026-terminal-renderer
    PRIVATE
    ${CMAKE_THREAD_LIBS_INIT}
    )

add_subdirectory(test)

