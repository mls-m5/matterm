cmake_minimum_required(VERSION 3.23)
project(matterm LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/Warnings.cmake)
option(MATTERM_BUILD_DEMO "Build the standalone matterm demos" ON)
option(MATTERM_BUILD_MATTRIX "Build mattrix, the standalone Mattrix OS terminal program (POSIX, no SDL)" ON)
option(MATTERM_BUILD_APP "Build the matterm terminal application" ON)
option(MATTERM_STATIC_DAEMON "Link matterm-daemon fully static (what --daemon-host copies to other computers; needs libsodium.a and libc.a)" ON)
if(MATTERM_STATIC_DAEMON)
    find_library(MATTERM_LIBSODIUM_STATIC NAMES libsodium.a)
    find_library(MATTERM_LIBC_STATIC NAMES libc.a)
    if(NOT MATTERM_LIBSODIUM_STATIC OR NOT MATTERM_LIBC_STATIC)
        message(WARNING "MATTERM_STATIC_DAEMON: libsodium.a or libc.a not found (apt: libsodium-dev libc6-dev); "
                        "building a dynamic matterm-daemon, which --daemon-host copies with a warning")
        set(MATTERM_STATIC_DAEMON OFF)
    endif()
endif()
option(MATTERM_BUILD_SERVER "Build the Linux HTTP/WebSocket server" OFF)
set(_matterm_graphics_default OFF)
if(MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO)
    set(_matterm_graphics_default ON)
endif()
option(MATTERM_BUILD_RENDERER "Build SDL renderer and graphical tests" ${_matterm_graphics_default})
if(MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO OR MATTERM_BUILD_RENDERER)
    find_package(SDL2 2.0.18 REQUIRED)
    find_package(SDL2_ttf 2.0.18 REQUIRED)
endif()

add_library(palette src/core/palette.cpp)
target_compile_features(palette PUBLIC cxx_std_17)
target_include_directories(palette PUBLIC src src/core)

if(UNIX AND NOT EMSCRIPTEN)
    # Saved sessions: /proc reader and session files (header-only logic is in src/frontend).
    add_library(sessionfiles src/platform/posix/procinfo.cpp src/platform/posix/sessionfiles.cpp)
    target_compile_features(sessionfiles PUBLIC cxx_std_17)
    target_include_directories(sessionfiles PUBLIC src/frontend src/platform/posix)
    target_link_libraries(sessionfiles PUBLIC palette)
endif()

# Common cells, storage, and presentation state: no terminal protocol or SDL.
add_library(cellbuffer src/core/cellbuffer.cpp)
target_compile_features(cellbuffer PUBLIC cxx_std_17)
target_include_directories(cellbuffer PUBLIC src src/core)
target_link_libraries(cellbuffer PUBLIC palette)

add_library(terminalmemory src/core/terminalmemory.cpp src/core/scrollviewport.cpp)
target_compile_features(terminalmemory PUBLIC cxx_std_17)
target_include_directories(terminalmemory PUBLIC src src/core)
target_link_libraries(terminalmemory PUBLIC palette)

# CPU screen behavior and stream decoding are independent library layers.
add_library(terminalmodel src/terminal/terminalmodel.cpp src/terminal/inputencoder.cpp)
add_library(matterm::terminal ALIAS terminalmodel)
target_include_directories(terminalmodel PUBLIC src/terminal)
target_link_libraries(terminalmodel PUBLIC terminalmemory)

# Wire protocol for remote terminals: pure data and codec, no I/O.
add_library(ttyprotocol src/protocol/message.cpp src/protocol/serversession.cpp)
add_library(matterm::protocol ALIAS ttyprotocol)
target_compile_features(ttyprotocol PUBLIC cxx_std_17)
target_include_directories(ttyprotocol PUBLIC src/protocol)

# Terminal links: message channel and link interfaces plus the remote client
# session. Portable C++17, no I/O, so the same code builds for the browser.
add_library(ttytransport src/transport/clientsession.cpp src/transport/scriptedlink.cpp)
add_library(matterm::transport ALIAS ttytransport)
target_compile_features(ttytransport PUBLIC cxx_std_17)
target_include_directories(ttytransport PUBLIC src/transport)
target_link_libraries(ttytransport PUBLIC matterm::protocol)
if(UNIX)
    add_library(ttyunix src/transport/unixchannel.cpp)
    add_library(matterm::unixchannel ALIAS ttyunix)
    target_compile_features(ttyunix PUBLIC cxx_std_17)
    target_include_directories(ttyunix PUBLIC src/transport)
endif()

# chat: the pure core of the chat (text rules, room, client). No I/O, clock or
# process access (enforced by test/check_fakeos_includes.cmake pointed at it).
file(GLOB CHAT_SOURCES CONFIGURE_DEPENDS src/chat/*.cpp)
add_library(chatcore ${CHAT_SOURCES})
add_library(matterm::chat ALIAS chatcore)
target_compile_features(chatcore PUBLIC cxx_std_17)
target_include_directories(chatcore PUBLIC src/chat)

# The commit this build comes from, shown in version messages and lists (never decides anything).
# Taken when cmake runs; "unknown" without git.
find_package(Git QUIET)
set(MATTERM_BUILD_HASH "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_status)
    if(git_status EQUAL 0 AND NOT git_hash STREQUAL "")
        set(MATTERM_BUILD_HASH ${git_hash})
        # Uncommitted changes to tracked files: the hash says so (the build is not that commit).
        execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE git_changes
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT git_changes STREQUAL "")
            set(MATTERM_BUILD_HASH "${git_hash}-dirty")
        endif()
    endif()
endif()
add_compile_definitions(MATTERM_BUILD_HASH="${MATTERM_BUILD_HASH}")

# tty: the terminal drawing layer (cell buffer in, smallest VT update out, colour
# reduction, key and mouse decoding) with no matterm types, meant to stand alone
# later. The pure part has no I/O (enforced by test/check_fakeos_includes.cmake);
# src/tty/posix is the POSIX end (raw mode, window size, write).
file(GLOB TTY_SOURCES CONFIGURE_DEPENDS src/tty/*.cpp)
add_library(tty ${TTY_SOURCES})
add_library(matterm::tty ALIAS tty)
target_compile_features(tty PUBLIC cxx_std_17)
target_include_directories(tty PUBLIC src)
if(UNIX)
    add_library(ttyposix src/tty/posix/terminal.cpp src/tty/posix/session.cpp)
    add_library(matterm::ttyposix ALIAS ttyposix)
    target_link_libraries(ttyposix PUBLIC matterm::tty)
endif()

# The only place the tty layer meets matterm's own cell buffer and cursor.
add_library(ttymatterm src/tty/matterm/cells.cpp src/tty/matterm/snapshot.cpp)
add_library(matterm::ttymatterm ALIAS ttymatterm)
target_link_libraries(ttymatterm PUBLIC matterm::tty cellbuffer matterm::terminal)

# fakeos: a pretend Unix system with ASCII animations. Pure C++17 with no I/O,
# clock or process access (enforced by test/check_fakeos_includes.cmake), so the
# same core runs in the browser, in the native window and in a plain terminal.
file(GLOB FAKEOS_SOURCES CONFIGURE_DEPENDS src/fakeos/*.cpp)
add_library(fakeos ${FAKEOS_SOURCES})
add_library(matterm::fakeos ALIAS fakeos)
target_compile_features(fakeos PUBLIC cxx_std_17)
target_include_directories(fakeos PUBLIC src/fakeos)
target_link_libraries(fakeos PUBLIC matterm::chat matterm::tty) # The chat program; the cell buffer.
# The `about` command prints numbers counted from the repository: a build step
# writes them to a header (cmake/AboutStats.cmake), so they match what was built.
set(FAKEOS_ABOUT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/fakeos_about_stats.h)
add_custom_target(fakeos_aboutstats
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${FAKEOS_ABOUT_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AboutStats.cmake
    BYPRODUCTS ${FAKEOS_ABOUT_HEADER}
    COMMENT "Counting the repository for `about`")
add_dependencies(fakeos fakeos_aboutstats)
target_include_directories(fakeos PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_library(utf8decoder src/terminal/vt/utf8decoder.cpp)
add_library(matterm::utf8 ALIAS utf8decoder)
target_compile_features(utf8decoder PUBLIC cxx_std_17)
target_include_directories(utf8decoder PUBLIC src src/terminal/vt)

add_library(vtparser src/terminal/vt/vtparser.cpp)
add_library(matterm::vt ALIAS vtparser)
target_include_directories(vtparser PUBLIC src/terminal/vt)
target_link_libraries(vtparser PUBLIC matterm::terminal matterm::utf8)

# Graphical resources and backends do not depend on terminal emulation.
if(MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO OR MATTERM_BUILD_RENDERER)
add_library(glyphatlas src/render/glyphatlas.cpp)
target_compile_features(glyphatlas PUBLIC cxx_std_17)
target_include_directories(glyphatlas PUBLIC src src/render)
target_link_libraries(glyphatlas PUBLIC SDL2::SDL2 SDL2_ttf::SDL2_ttf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(MATTERM_SYSTEM_FONT_FALLBACK "Use Fontconfig for missing glyphs" ON)
    if(MATTERM_SYSTEM_FONT_FALLBACK)
        find_package(Fontconfig QUIET)
        if(Fontconfig_FOUND)
            target_link_libraries(glyphatlas PRIVATE Fontconfig::Fontconfig)
            target_compile_definitions(glyphatlas PRIVATE MATTERM_HAS_FONTCONFIG=1)
        else()
            message(STATUS "Fontconfig development files not found; system glyph fallback disabled")
        endif()
    endif()
endif()

add_library(sdlbackend src/render/sdlbackend.cpp)
target_link_libraries(sdlbackend PUBLIC glyphatlas cellbuffer)

add_library(matterm-renderer INTERFACE)
add_library(matterm::renderer ALIAS matterm-renderer)
target_link_libraries(matterm-renderer INTERFACE cellbuffer terminalmemory sdlbackend)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(MATTERM_ENABLE_PTY "Build the Linux PTY adapter" ON)
else()
    option(MATTERM_ENABLE_PTY "Build the Linux PTY adapter" OFF)
endif()
if(MATTERM_ENABLE_PTY)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The current PTY adapter is Linux-only")
    endif()
    find_library(UTIL_LIBRARY util REQUIRED)
    add_library(ptyprocess src/platform/posix/ptyprocess.cpp)
    add_library(matterm::pty ALIAS ptyprocess)
    target_compile_features(ptyprocess PUBLIC cxx_std_17)
    target_include_directories(ptyprocess PUBLIC src src/platform/posix)
    target_link_libraries(ptyprocess PRIVATE ${UTIL_LIBRARY})
    add_library(ptylink src/platform/posix/ptylink.cpp)
    add_library(matterm::ptylink ALIAS ptylink)
    target_link_libraries(ptylink PUBLIC matterm::pty matterm::transport)
    add_library(ptysession src/platform/posix/ptysession.cpp)
    add_library(matterm::session ALIAS ptysession)
    target_link_libraries(ptysession PUBLIC matterm::ptylink matterm::vt)
endif()

if(MATTERM_BUILD_SERVER)
    add_subdirectory(src/server)
    add_subdirectory(src/daemon)
endif()
include(cmake/BundledFont.cmake)
if(MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO)
    add_library(matterm-appsupport app/support/fontpath.cpp)
    target_compile_features(matterm-appsupport PUBLIC cxx_std_17)
    target_include_directories(matterm-appsupport PUBLIC app/support)
    target_link_libraries(matterm-appsupport PUBLIC SDL2::SDL2)
    if(MATTERM_BUNDLE_FONT)
        target_compile_definitions(matterm-appsupport PRIVATE MATTERM_HAS_BUNDLED_FONT=1)
    endif()
endif()

if(MATTERM_BUILD_DEMO OR BUILD_TESTING)
    add_library(democontent demo/democontent.cpp)
    target_include_directories(democontent PUBLIC demo)
    target_link_libraries(democontent PUBLIC cellbuffer terminalmemory)
endif()

if(MATTERM_BUILD_DEMO)
    add_executable(matterm-render-demo demo/main.cpp)
    target_link_libraries(matterm-render-demo PRIVATE matterm::renderer democontent)
    matterm_application_assets(matterm-render-demo)
    add_executable(matterm-lib-demo demo/terminalmain.cpp)
    target_link_libraries(matterm-lib-demo PRIVATE matterm::terminal matterm::utf8 matterm::renderer democontent)
    matterm_application_assets(matterm-lib-demo)
endif()
if(MATTERM_BUILD_DEMO)
    # The pretend system (src/fakeos) in a window, or with --tty in a terminal.
    add_executable(matterm-demo app/demo/main.cpp)
    target_sources(matterm-demo PRIVATE app/demo/beeper.cpp)
    target_include_directories(matterm-demo PRIVATE src/frontend app/demo)
    target_link_libraries(matterm-demo PRIVATE matterm::fakeos matterm::transport matterm::vt matterm::renderer)
    matterm_application_assets(matterm-demo)
    if(UNIX)
        target_sources(matterm-demo PRIVATE app/demo/ttyhost.cpp)
        target_link_libraries(matterm-demo PRIVATE matterm::ttyposix)
        target_include_directories(matterm-demo PRIVATE app/demo)
        target_compile_definitions(matterm-demo PRIVATE MATTERM_DEMO_TTY=1)
    endif()
endif()
if(MATTERM_BUILD_MATTRIX AND UNIX AND NOT EMSCRIPTEN)
    # Mattrix OS in the terminal it was started from: the fakeos core and the
    # raw terminal host, nothing graphical.
    add_executable(mattrix app/mattrix/main.cpp app/demo/ttyhost.cpp)
    target_include_directories(mattrix PRIVATE app/demo)
    target_link_libraries(mattrix PRIVATE matterm::fakeos matterm::ttyposix)
    install(TARGETS mattrix RUNTIME DESTINATION bin)
endif()
option(MATTERM_BUILD_CHATD "Build chatd, the chat server (needs Boost, POSIX)" ON)
if(MATTERM_BUILD_CHATD AND UNIX AND NOT EMSCRIPTEN)
    find_package(Boost 1.74 QUIET)
    find_package(Threads QUIET)
    if(Boost_FOUND AND Threads_FOUND)
        # The server core: the chat room plus Beast. Reuses only the proxy and
        # source-key helpers of matterm-server (no sodium, no fast-json).
        add_library(chatd-core src/chatd/chatserver.cpp
                    src/server/proxysource.cpp src/server/ratelimiter.cpp)
        target_compile_features(chatd-core PUBLIC cxx_std_17)
        target_include_directories(chatd-core PUBLIC src/chatd src/server ${Boost_INCLUDE_DIRS})
        target_compile_definitions(chatd-core PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
        target_link_libraries(chatd-core PUBLIC matterm::chat Threads::Threads)
        add_executable(chatd src/chatd/main.cpp)
        target_link_libraries(chatd PRIVATE chatd-core)
        # A fully static binary for a container without anything else in it.
        option(MATTERM_CHATD_STATIC "Link chatd statically (for the Podman image)" OFF)
        if(MATTERM_CHATD_STATIC)
            target_link_options(chatd PRIVATE -static)
        endif()
        install(TARGETS chatd RUNTIME DESTINATION bin)
        # The chat in a terminal: the chat library, the terminal helpers and the
        # key decoder of fakeos. TCP and plain WebSocket (no TLS yet).
        add_executable(mattrix-chat app/chat/main.cpp)
        target_include_directories(mattrix-chat PRIVATE app/demo)
        target_link_libraries(mattrix-chat PRIVATE matterm::chat matterm::fakeos matterm::ttyposix chatd-core)
        install(TARGETS mattrix-chat RUNTIME DESTINATION bin)
    else()
        message(STATUS "chatd: Boost 1.74+ or threads not found, not built")
    endif()
endif()
# `matterm --attach-tty` without SDL (docs/tasks/daemon-versioning.md): the mode, its daemon
# client and the shared tty layer. matterm links it; a program without SDL (matmux) can too.
# Entry point: runAttachTty(DaemonOptions) in app/attachtty.h.
if(UNIX AND TARGET ptysession)
    add_library(attachtty app/attachtty.cpp app/daemoncli.cpp src/daemon/control.cpp src/daemon/buildhash.cpp
                      src/daemon/socketpath.cpp)
    add_library(matterm::attachtty ALIAS attachtty)
    target_compile_features(attachtty PUBLIC cxx_std_17)
    target_include_directories(attachtty PUBLIC app src/frontend src/daemon)
    target_link_libraries(attachtty PUBLIC matterm::transport matterm::unixchannel matterm::vt matterm::terminal
                                           matterm::ttyposix matterm::ttymatterm)
endif()

# matmux: matterm --attach-tty plus --daemon-list/--daemon-stop in one file without SDL,
# static where the daemon is (what --daemon-install copies next to matterm-daemon).
if(TARGET attachtty AND UNIX)
    add_executable(matmux app/matmux/main.cpp)
    target_link_libraries(matmux PRIVATE matterm::attachtty)
    if(MATTERM_STATIC_DAEMON)
        target_link_options(matmux PRIVATE -static)
    endif()
    set_target_properties(matmux PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    install(TARGETS matmux RUNTIME DESTINATION bin)
endif()

if(MATTERM_BUILD_APP)
    add_executable(matterm app/main.cpp)
    target_include_directories(matterm PRIVATE src/frontend app)
    target_link_libraries(matterm PRIVATE matterm::vt matterm::renderer)
    matterm_application_assets(matterm)
    if(TARGET ptysession)
        target_sources(matterm PRIVATE app/ptyfrontend.cpp)
        target_include_directories(matterm PRIVATE src/frontend)
        target_link_libraries(matterm PRIVATE matterm::session sessionfiles)
        target_compile_definitions(matterm PRIVATE MATTERM_HAS_PTY=1)
        if(UNIX)
            # The daemon's control messages (a codec only: no server code) and its socket.
            target_link_libraries(matterm PRIVATE matterm::attachtty)
            target_include_directories(matterm PRIVATE src/daemon)
            target_compile_definitions(matterm PRIVATE MATTERM_HAS_ATTACH_TTY=1)
            target_link_libraries(matterm PRIVATE matterm::unixchannel matterm::transport)
        endif()
    endif()
endif()
option(MATTERM_BUILD_BENCHMARKS "Build uninstrumented deterministic performance tools" OFF)
if(MATTERM_BUILD_BENCHMARKS)
    add_library(matterm-bench-workload bench/workload.cpp)
    target_compile_features(matterm-bench-workload PUBLIC cxx_std_17)
    target_include_directories(matterm-bench-workload PUBLIC bench)
    add_executable(matterm-parser-bench bench/parserbench.cpp)
    target_link_libraries(matterm-parser-bench PRIVATE matterm-bench-workload matterm::vt)
    if(UNIX AND NOT EMSCRIPTEN)
        add_executable(matterm-terminal-bench bench/terminalbench.cpp)
        target_link_libraries(matterm-terminal-bench PRIVATE matterm-bench-workload)
        add_executable(matterm-reflow-bench bench/reflowbench.cpp)
        target_link_libraries(matterm-reflow-bench PRIVATE terminalmemory)
        add_executable(matterm-model-bench bench/modelbench.cpp)
        target_link_libraries(matterm-model-bench PRIVATE matterm::terminal)
        target_compile_definitions(matterm-model-bench PRIVATE MATTERM_HAS_REFLOW)
        if(TARGET matterm-daemon-core)
            # Reattach/replay: where the time goes (docs/CPU.md "Replay").
            add_executable(matterm-replay-bench bench/replaybench.cpp)
            target_include_directories(matterm-replay-bench PRIVATE src/frontend)
            target_link_libraries(matterm-replay-bench PRIVATE matterm::ttymatterm matterm-daemon-core matterm::unixchannel matterm::transport
                                                              matterm::vt matterm::terminal)
        endif()
    endif()
endif()
add_subdirectory(lib)
add_subdirectory(test)
