# Browser build of the matterm terminal (Emscripten). A separate top-level
# project on purpose: it reuses the sources under src/ without touching the
# native root CMakeLists.txt, which finds SDL2 as a system package.
#
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -S web -B build-web -DMATTERM_FONT=/path/JetBrainsMono-Regular.ttf
#   cmake --build build-web
#   (serve build-web/ over HTTP and open matterm-web.html; add
#   ?ws=ws://host:port/path to connect to a matterm-tty server, else a demo)
cmake_minimum_required(VERSION 3.23)
project(matterm-web LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (see the comment at the top).")
endif()

set(MATTERM_FONT "" CACHE FILEPATH "TrueType font bundled into the page")
if(NOT MATTERM_FONT OR NOT EXISTS "${MATTERM_FONT}")
    message(FATAL_ERROR "Set -DMATTERM_FONT=<path to JetBrainsMono-Regular.ttf>")
endif()

set(root "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# SDL2 and SDL2_ttf come from Emscripten's ports, selected by these flags at
# both compile and link time.
set(sdl_flags -sUSE_SDL=2 -sUSE_SDL_TTF=2)

add_library(matterm-web-core STATIC
    ${root}/src/core/palette.cpp
    ${root}/src/core/cellbuffer.cpp
    ${root}/src/core/terminalmemory.cpp
    ${root}/src/core/scrollviewport.cpp
    ${root}/src/terminal/terminalmodel.cpp
    ${root}/src/terminal/inputencoder.cpp
    ${root}/src/terminal/vt/utf8decoder.cpp
    ${root}/src/terminal/vt/vtparser.cpp
    ${root}/src/render/glyphatlas.cpp
    ${root}/src/render/sdlbackend.cpp
    ${root}/src/protocol/message.cpp
    ${root}/src/transport/clientsession.cpp
)
target_include_directories(matterm-web-core PUBLIC
    ${root}/src ${root}/src/core ${root}/src/render ${root}/src/terminal
    ${root}/src/terminal/vt ${root}/src/protocol ${root}/src/transport)
target_compile_options(matterm-web-core PUBLIC ${sdl_flags})
target_link_options(matterm-web-core PUBLIC ${sdl_flags})

add_executable(matterm-web main.cpp websocketchannel.cpp)
target_link_libraries(matterm-web PRIVATE matterm-web-core)
set_target_properties(matterm-web PROPERTIES SUFFIX ".html")
target_link_options(matterm-web PRIVATE
    -sALLOW_MEMORY_GROWTH=1
    -sEXIT_RUNTIME=0
    -lwebsocket.js
    "--preload-file=${MATTERM_FONT}@/assets/font.ttf")
