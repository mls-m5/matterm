if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET matterm-server-core)
    message(FATAL_ERROR "matterm-daemon needs Linux and the server core (MATTERM_BUILD_SERVER=ON)")
endif()
add_library(matterm-daemon-core daemon.cpp)
target_compile_features(matterm-daemon-core PUBLIC cxx_std_20)
target_include_directories(matterm-daemon-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matterm-daemon-core PUBLIC matterm-daemon-wire matterm-server-core sessionfiles)
add_executable(matterm-daemon main.cpp)
target_link_libraries(matterm-daemon PRIVATE matterm-daemon-core matterm::unixchannel matterm::transport)
target_include_directories(matterm-daemon PRIVATE ${PROJECT_SOURCE_DIR}/src/frontend)
if(MATTERM_STATIC_DAEMON)
    # One file that runs on any Linux of the same architecture, no packages needed: this is what
    # `matterm --daemon-host` copies to another computer (docs/REMOTE_DAEMON.md). libsodium is static
    # too (src/server/CMakeLists.txt). The daemon uses no name service lookups (no getpwnam, no DNS).
    target_link_options(matterm-daemon PRIVATE -static)
endif()
# Next to the matterm program, where it looks for the daemon (build/matterm-daemon).
if(TARGET matterm OR EXISTS "${CMAKE_SOURCE_DIR}/app/main.cpp")
    set_target_properties(matterm-daemon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
# Test support for the browser end-to-end tests of the sharing path. Not installed, not for users.
add_executable(matterm-daemon-e2e e2e_helper.cpp)
target_link_libraries(matterm-daemon-e2e PRIVATE matterm-daemon-core)
