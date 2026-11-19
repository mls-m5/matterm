cmake_minimum_required(VERSION 3.23)
project(matterm LANGUAGES CXX)

find_package(SDL2 2.0.18 REQUIRED)
find_package(SDL2_ttf 2.0.18 REQUIRED)

# Common cells, storage, and presentation state: no terminal protocol or SDL.
add_library(cellbuffer src/core/cellbuffer.cpp)
target_compile_features(cellbuffer PUBLIC cxx_std_17)
target_include_directories(cellbuffer PUBLIC src src/core)

add_library(terminalmemory src/core/terminalmemory.cpp src/core/scrollviewport.cpp)
target_compile_features(terminalmemory PUBLIC cxx_std_17)
target_include_directories(terminalmemory PUBLIC src src/core)

# CPU screen behavior and stream decoding are independent library layers.
add_library(terminalmodel src/terminal/terminalmodel.cpp)
add_library(matterm::terminal ALIAS terminalmodel)
target_include_directories(terminalmodel PUBLIC src/terminal)
target_link_libraries(terminalmodel PUBLIC terminalmemory)

add_library(utf8decoder src/terminal/vt/utf8decoder.cpp)
add_library(matterm::utf8 ALIAS utf8decoder)
target_compile_features(utf8decoder PUBLIC cxx_std_17)
target_include_directories(utf8decoder PUBLIC src src/terminal/vt)

add_library(vtparser src/terminal/vt/vtparser.cpp)
add_library(matterm::vt ALIAS vtparser)
target_include_directories(vtparser PUBLIC src/terminal/vt)
target_link_libraries(vtparser PUBLIC matterm::terminal matterm::utf8)

# Graphical resources and backends do not depend on terminal emulation.
add_library(glyphatlas src/render/glyphatlas.cpp)
target_compile_features(glyphatlas PUBLIC cxx_std_17)
target_include_directories(glyphatlas PUBLIC src src/render)
target_link_libraries(glyphatlas PUBLIC SDL2::SDL2 SDL2_ttf::SDL2_ttf)

add_library(sdlbackend src/render/sdlbackend.cpp)
target_link_libraries(sdlbackend PUBLIC glyphatlas cellbuffer)

add_library(matterm-renderer INTERFACE)
add_library(matterm::renderer ALIAS matterm-renderer)
target_link_libraries(matterm-renderer INTERFACE cellbuffer terminalmemory sdlbackend)

include(CTest)
option(MATTERM_BUILD_DEMO "Build the standalone matterm demos" ON)
option(MATTERM_BUILD_APP "Build the matterm VT frontend (PTY not implemented yet)" ON)

if(MATTERM_BUILD_DEMO OR BUILD_TESTING)
    add_library(democontent demo/democontent.cpp)
    target_include_directories(democontent PUBLIC demo)
    target_link_libraries(democontent PUBLIC cellbuffer terminalmemory)
endif()

if(MATTERM_BUILD_DEMO)
    add_executable(matterm-demo demo/main.cpp)
    target_link_libraries(matterm-demo PRIVATE matterm::renderer democontent)
    add_executable(matterm-terminal-demo demo/terminalmain.cpp)
    target_link_libraries(matterm-terminal-demo PRIVATE matterm::terminal matterm::utf8 matterm::renderer democontent)
endif()
if(MATTERM_BUILD_APP)
    add_executable(matterm app/main.cpp)
    target_link_libraries(matterm PRIVATE matterm::vt matterm::renderer)
endif()
add_subdirectory(lib)
add_subdirectory(test)
