# Browser build of the matterm terminal (Emscripten). A separate top-level
# project on purpose: it reuses the sources under src/ without touching the
# native root CMakeLists.txt, which finds SDL2 as a system package.
#
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -S web -B build-web -DMATTERM_FONT=/path/JetBrainsMono-Regular.ttf
#   cmake --build build-web
#   matterm-server --static-dir build-web/site ...   (see docs/SERVER.md)
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

file(GLOB fakeos_sources CONFIGURE_DEPENDS ${root}/src/fakeos/*.cpp)
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
    ${root}/src/transport/scriptedlink.cpp
    ${fakeos_sources}
)
target_include_directories(matterm-web-core PUBLIC
    ${root}/src ${root}/src/core ${root}/src/render ${root}/src/terminal
    ${root}/src/terminal/vt ${root}/src/protocol ${root}/src/transport
    ${root}/src/fakeos
    ${root}/src/frontend)
target_compile_options(matterm-web-core PUBLIC ${sdl_flags})
target_link_options(matterm-web-core PUBLIC ${sdl_flags})

add_executable(matterm-web main.cpp websocketchannel.cpp)
target_link_libraries(matterm-web PRIVATE matterm-web-core)
# <build>/site/ (index.html, index.js/.wasm/.data, login.js, style.css) is what
# matterm-server --static-dir serves.
set_target_properties(matterm-web PROPERTIES
    OUTPUT_NAME "index" SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/site")
# No Emscripten HTML shell: the page is ours (shell.html becomes index.html)
# and loads login.js, which starts index.js after login. Plain files only, as
# the server's CSP forbids inline script and style.
add_custom_command(TARGET matterm-web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/login.js ${CMAKE_CURRENT_SOURCE_DIR}/style.css
        ${CMAKE_CURRENT_SOURCE_DIR}/embed.js
        $<TARGET_FILE_DIR:matterm-web>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
        $<TARGET_FILE_DIR:matterm-web>/index.html)
target_link_options(matterm-web PRIVATE
    -sALLOW_MEMORY_GROWTH=1
    -sEXIT_RUNTIME=0
    -sEXPORTED_FUNCTIONS=_main,_matterm_paste,_matterm_text,_matterm_key
    -sEXPORTED_RUNTIME_METHODS=ccall
    -lwebsocket.js
    "--preload-file=${MATTERM_FONT}@/assets/font.ttf")

# The POST_BUILD copy above only runs when index.js is relinked. This target
# refreshes the plain files (page, script, style) on every build, so editing
# login.js, style.css or shell.html needs no relink.
add_custom_target(matterm-web-site ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/login.js ${CMAKE_CURRENT_SOURCE_DIR}/style.css
        ${CMAKE_CURRENT_SOURCE_DIR}/embed.js
        $<TARGET_FILE_DIR:matterm-web>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
        $<TARGET_FILE_DIR:matterm-web>/index.html)
add_dependencies(matterm-web-site matterm-web)

# demo.html: the page with a marker that selects the in-browser demo, so a static
# host (GitHub Pages, see .github/workflows/pages.yml) can serve it as index.html
# without a login form or a query string.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shell.html matterm_shell_html)
string(REPLACE "<title>matterm</title>"
    "<title>matterm demo</title>\n<meta name=\"matterm-static-demo\" content=\"1\">"
    matterm_demo_html "${matterm_shell_html}")
file(WRITE ${CMAKE_BINARY_DIR}/demo.html "${matterm_demo_html}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
add_custom_target(matterm-web-demo-page ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/demo.html
        $<TARGET_FILE_DIR:matterm-web>/demo.html)
add_dependencies(matterm-web-demo-page matterm-web)
