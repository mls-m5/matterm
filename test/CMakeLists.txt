if(BUILD_TESTING)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mls-unit-test/test_main.cpp")
        message(FATAL_ERROR "Run git submodule update --init to fetch mls-unit-test, or configure with -DBUILD_TESTING=OFF.")
    endif()

    add_executable(cellgridview_test
        cellgridview_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_compile_features(cellgridview_test PRIVATE cxx_std_17)
    target_include_directories(cellgridview_test PRIVATE ../src/core)
    add_test(NAME cell_grid_view COMMAND cellgridview_test)

    add_executable(cellbuffer_test
        cellbuffer_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(cellbuffer_test PRIVATE cellbuffer)
    add_test(NAME cell_buffer COMMAND cellbuffer_test)

    add_executable(terminalmemory_test
        terminalmemory_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(terminalmemory_test PRIVATE terminalmemory)
    add_test(NAME terminal_memory COMMAND terminalmemory_test)

    add_executable(terminalmodel_test
        terminalmodel_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(terminalmodel_test PRIVATE matterm::terminal)
    add_test(NAME terminal_model COMMAND terminalmodel_test)

    add_executable(utf8decoder_test
        utf8decoder_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(utf8decoder_test PRIVATE matterm::utf8)
    add_test(NAME utf8_decoder COMMAND utf8decoder_test)

    add_executable(vtparser_test
        vtparser_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(vtparser_test PRIVATE matterm::vt)
    add_test(NAME vt_parser COMMAND vtparser_test)

    add_executable(terminaloperations_test terminaloperations_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(terminaloperations_test PRIVATE matterm::vt)
    add_test(NAME terminal_operations COMMAND terminaloperations_test)

    add_executable(urllink_test urllink_test.cpp mls-unit-test/test_main.cpp)
    target_include_directories(urllink_test PRIVATE ../src/frontend)
    target_link_libraries(urllink_test PRIVATE matterm::terminal)
    add_test(NAME url_link COMMAND urllink_test)

    add_executable(textselection_test textselection_test.cpp mls-unit-test/test_main.cpp)
    target_include_directories(textselection_test PRIVATE ../src/frontend)
    target_link_libraries(textselection_test PRIVATE matterm::terminal)
    add_test(NAME text_selection COMMAND textselection_test)

    add_executable(cursorglide_test cursorglide_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(cursorglide_test PRIVATE cellbuffer)
    add_test(NAME cursor_glide COMMAND cursorglide_test)

    add_executable(palette_test palette_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(palette_test PRIVATE matterm::vt)
    add_test(NAME palette COMMAND palette_test)

    add_executable(inputencoder_test inputencoder_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(inputencoder_test PRIVATE matterm::vt)
    add_test(NAME input_encoder COMMAND inputencoder_test)

    add_executable(protocol_test protocol_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(protocol_test PRIVATE matterm::protocol)
    add_test(NAME protocol COMMAND protocol_test)

    add_executable(protocolfuzz_test protocolfuzz_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(protocolfuzz_test PRIVATE matterm::transport)
    add_test(NAME protocol_fuzz COMMAND protocolfuzz_test)
    add_executable(vtfuzz_test vtfuzz_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(vtfuzz_test PRIVATE matterm::vt)
    add_test(NAME vt_fuzz COMMAND vtfuzz_test)
    set_tests_properties(protocol_fuzz vt_fuzz PROPERTIES TIMEOUT 10)
    if(TARGET matterm-server-core)
        add_executable(serverfuzz_test serverfuzz_test.cpp mls-unit-test/test_main.cpp)
        target_link_libraries(serverfuzz_test PRIVATE matterm-server-core)
        add_test(NAME server_fuzz COMMAND serverfuzz_test)
        set_tests_properties(server_fuzz PROPERTIES TIMEOUT 10)
    endif()

    add_executable(serversession_test serversession_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(serversession_test PRIVATE matterm::protocol)
    add_test(NAME server_session COMMAND serversession_test)

    add_executable(clientsession_test clientsession_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(clientsession_test PRIVATE matterm::transport)
    add_test(NAME client_session COMMAND clientsession_test)

    add_executable(scriptedlink_test scriptedlink_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(scriptedlink_test PRIVATE matterm::transport matterm::vt)
    add_test(NAME scripted_link COMMAND scriptedlink_test)

    add_executable(windowsnap_test windowsnap_test.cpp mls-unit-test/test_main.cpp)
    target_include_directories(windowsnap_test PRIVATE ${PROJECT_SOURCE_DIR}/src/frontend)
    add_test(NAME window_snap COMMAND windowsnap_test)

    file(GLOB FAKEOS_TESTS CONFIGURE_DEPENDS fakeos_*_test.cpp)
    add_executable(fakeos_test ${FAKEOS_TESTS} mls-unit-test/test_main.cpp)
    target_link_libraries(fakeos_test PRIVATE matterm::fakeos matterm::vt matterm::transport)
    # The about command's numbers are re-counted from the sources by its test.
    target_compile_definitions(fakeos_test PRIVATE MATTERM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME fakeos COMMAND fakeos_test)
    add_test(NAME fakeos_includes
        COMMAND ${CMAKE_COMMAND} -DFAKEOS_DIR=${PROJECT_SOURCE_DIR}/src/fakeos
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_fakeos_includes.cmake)

    add_executable(touchgesture_test touchgesture_test.cpp mls-unit-test/test_main.cpp)
    target_include_directories(touchgesture_test PRIVATE ${PROJECT_SOURCE_DIR}/src/frontend)
    add_test(NAME touch_gesture COMMAND touchgesture_test)

    add_executable(scrollviewport_test
        scrollviewport_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(scrollviewport_test PRIVATE terminalmemory)
    add_test(NAME scroll_viewport COMMAND scrollviewport_test)

    add_executable(democontent_test
        democontent_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(democontent_test PRIVATE democontent)
    add_test(NAME demo_content COMMAND democontent_test)

    if(TARGET glyphatlas)
    add_executable(glyphatlas_test
        glyphatlas_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(glyphatlas_test PRIVATE glyphatlas)

    add_executable(sdlbackend_test
        sdlbackend_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(sdlbackend_test PRIVATE cellbuffer terminalmemory sdlbackend)
    endif()

    if(TARGET ptyprocess)
        add_executable(pty_test_child pty_test_child.cpp)
        target_compile_features(pty_test_child PRIVATE cxx_std_17)
        add_executable(ptyprocess_test ptyprocess_test.cpp mls-unit-test/test_main.cpp)
        target_link_libraries(ptyprocess_test PRIVATE matterm::pty)
        target_compile_definitions(ptyprocess_test PRIVATE PTY_TEST_CHILD="$<TARGET_FILE:pty_test_child>")
        add_dependencies(ptyprocess_test pty_test_child)
        add_test(NAME pty_process COMMAND ptyprocess_test)
        set_tests_properties(pty_process PROPERTIES TIMEOUT 20)
        add_executable(ptylink_test ptylink_test.cpp mls-unit-test/test_main.cpp)
        target_link_libraries(ptylink_test PRIVATE matterm::ptylink)
        target_compile_definitions(ptylink_test PRIVATE PTY_TEST_CHILD="$<TARGET_FILE:pty_test_child>")
        add_dependencies(ptylink_test pty_test_child)
        add_test(NAME pty_link COMMAND ptylink_test)
        set_tests_properties(pty_link PROPERTIES TIMEOUT 20)
        add_executable(ptysession_test ptysession_test.cpp mls-unit-test/test_main.cpp)
        target_link_libraries(ptysession_test PRIVATE matterm::session)
        target_compile_definitions(ptysession_test PRIVATE PTY_TEST_CHILD="$<TARGET_FILE:pty_test_child>")
        add_dependencies(ptysession_test pty_test_child)
        find_program(MATTERM_TEST_BASH bash)
        if(MATTERM_TEST_BASH)
            target_compile_definitions(ptysession_test PRIVATE MATTERM_TEST_BASH="${MATTERM_TEST_BASH}")
        endif()
        find_program(MATTERM_TEST_TOP top)
        if(MATTERM_TEST_TOP)
            target_compile_definitions(ptysession_test PRIVATE MATTERM_TEST_TOP="${MATTERM_TEST_TOP}")
        endif()
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import curses"
                RESULT_VARIABLE MATTERM_CURSES_RESULT OUTPUT_QUIET ERROR_QUIET)
            if(MATTERM_CURSES_RESULT EQUAL 0)
                target_compile_definitions(ptysession_test PRIVATE
                    MATTERM_TEST_PYTHON="${Python3_EXECUTABLE}"
                    MATTERM_TEST_CURSES_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/curses_test_child.py")
            endif()
        endif()
        add_test(NAME pty_session COMMAND ptysession_test)
        set_tests_properties(pty_session PROPERTIES TIMEOUT 20)
    endif()

    if(TARGET matterm-terminal-bench)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME terminal_bench_dsr_smoke
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/terminalbench_smoke.py"
                    $<TARGET_FILE:matterm-terminal-bench>)
            set_tests_properties(terminal_bench_dsr_smoke PROPERTIES TIMEOUT 15)
        endif()
    endif()
    if(TARGET matterm-parser-bench)
        foreach(workload IN ITEMS long short color redraw)
            add_test(NAME parser_bench_${workload}_smoke
                COMMAND matterm-parser-bench ${workload} 8192 17 1)
        endforeach()
    endif()

    if(TARGET matterm-server-core)
        foreach(name IN ITEMS serverauth serverratelimiter serverintegration serverlifecycle serverconfig serversecurity)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}_test.cpp")
                add_executable(${name}_test ${name}_test.cpp mls-unit-test/test_main.cpp)
                target_link_libraries(${name}_test PRIVATE matterm-server-core)
                add_test(NAME ${name} COMMAND ${name}_test)
                set_tests_properties(${name} PROPERTIES TIMEOUT 60)
                target_compile_definitions(${name}_test PRIVATE MATTERM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
            endif()
        endforeach()
    endif()

    if(TARGET matterm)
        add_test(NAME terminal_cli_help
            COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:matterm>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_cli.cmake")
        set_tests_properties(terminal_cli_help PROPERTIES TIMEOUT 10)
    endif()

    set(TEST_FONT_FILE "" CACHE FILEPATH "TrueType font for atlas and rendering tests")
    if(NOT TEST_FONT_FILE AND MATTERM_ASSET_JetBrainsMono-Regular.ttf)
        set(TEST_FONT_FILE "${MATTERM_ASSET_JetBrainsMono-Regular.ttf}")
    endif()
    if(TEST_FONT_FILE AND TARGET glyphatlas)
        if(NOT EXISTS "${TEST_FONT_FILE}")
            message(FATAL_ERROR "TEST_FONT_FILE does not exist: ${TEST_FONT_FILE}")
        endif()
        add_test(NAME glyph_atlas COMMAND glyphatlas_test)
        set_tests_properties(glyph_atlas PROPERTIES
            ENVIRONMENT "GLYPH_TEST_FONT=${TEST_FONT_FILE}")
        add_test(NAME sdl_backend COMMAND sdlbackend_test)
        set_tests_properties(sdl_backend PROPERTIES
            ENVIRONMENT "GLYPH_TEST_FONT=${TEST_FONT_FILE};SDL_VIDEODRIVER=dummy")
        if(TARGET matterm-render-demo)
            add_test(NAME glyph_render_smoke
                COMMAND matterm-render-demo "${TEST_FONT_FILE}" --smoke-test)
            set_tests_properties(glyph_render_smoke PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
        if(TARGET matterm-demo AND TARGET ptylink)
            add_executable(demotty_test demotty_test.cpp mls-unit-test/test_main.cpp)
            target_link_libraries(demotty_test PRIVATE matterm::ptylink)
            target_compile_definitions(demotty_test PRIVATE DEMO_BINARY="$<TARGET_FILE:matterm-demo>")
            add_dependencies(demotty_test matterm-demo)
            add_test(NAME demo_tty COMMAND demotty_test)
            set_tests_properties(demo_tty PROPERTIES TIMEOUT 60)
        endif()
        if(TARGET matterm-demo)
            add_test(NAME demo_render_smoke
                COMMAND matterm-demo "${TEST_FONT_FILE}" --smoke-test)
            set_tests_properties(demo_render_smoke PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
        endif()
            add_test(NAME terminal_model_render_smoke
                COMMAND matterm-lib-demo "${TEST_FONT_FILE}" --smoke-test)
            set_tests_properties(terminal_model_render_smoke PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
        endif()
        if(TARGET matterm)
            add_test(NAME vt_frontend_render_smoke
                COMMAND matterm "${TEST_FONT_FILE}" --smoke-test)
            set_tests_properties(vt_frontend_render_smoke PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
            add_test(NAME vt_frontend_font_size_12
                COMMAND matterm "${TEST_FONT_FILE}" --font-size 12 --smoke-test)
            set_tests_properties(vt_frontend_font_size_12 PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
            if(TARGET ptysession)
                add_test(NAME pty_frontend_render_smoke
                    COMMAND matterm "${TEST_FONT_FILE}" --font-size 12 --pty-smoke-test -- $<TARGET_FILE:pty_test_child> vt)
                set_tests_properties(pty_frontend_render_smoke PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 15)
                add_test(NAME pty_frontend_smooth_cursor_smoke
                    COMMAND matterm "${TEST_FONT_FILE}" --smooth-cursor -- /bin/sh -c
                        "printf 'cursor'; sleep 0.1; printf '\\r\\nmove'; sleep 0.1; test \"$1\" = --help" sh --help)
                set_tests_properties(pty_frontend_smooth_cursor_smoke PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 15)
                add_test(NAME pty_frontend_closes_on_exit
                    COMMAND matterm "${TEST_FONT_FILE}" -- /bin/sh -c "exit 0")
                set_tests_properties(pty_frontend_closes_on_exit PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 5)
                add_test(NAME pty_frontend_advertises_truecolor
                    COMMAND matterm "${TEST_FONT_FILE}" --term xterm-256color -- /bin/sh -c
                        "test \"$COLORTERM\" = truecolor && test \"$TERM\" = xterm-256color")
                set_tests_properties(pty_frontend_advertises_truecolor PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;COLORTERM=stale" TIMEOUT 5)
                add_test(NAME pty_frontend_reports_palette
                    COMMAND matterm "${TEST_FONT_FILE}" -- $<TARGET_FILE:pty_test_child> colors)
                set_tests_properties(pty_frontend_reports_palette PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 5)
                add_test(NAME pty_frontend_records_raw_output
                    COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:matterm>" "-DFONT=${TEST_FONT_FILE}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/pty_recording.cmake")
                set_tests_properties(pty_frontend_records_raw_output PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 15)
            endif()
        endif()
        if(MATTERM_BUNDLE_FONT)
            foreach(app IN ITEMS matterm matterm-render-demo matterm-lib-demo matterm-demo)
                if(TARGET ${app})
                    add_test(NAME ${app}_default_font COMMAND ${app} --smoke-test)
                    set_tests_properties(${app}_default_font PROPERTIES
                        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
                endif()
            endforeach()
        endif()
    else()
        message(STATUS "Set TEST_FONT_FILE to enable font-dependent atlas and rendering tests")
    endif()
endif()
