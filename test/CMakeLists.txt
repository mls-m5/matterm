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

    add_executable(palette_test palette_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(palette_test PRIVATE matterm::vt)
    add_test(NAME palette COMMAND palette_test)

    add_executable(inputencoder_test inputencoder_test.cpp mls-unit-test/test_main.cpp)
    target_link_libraries(inputencoder_test PRIVATE matterm::vt)
    add_test(NAME input_encoder COMMAND inputencoder_test)

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

    if(TARGET ptyprocess)
        add_executable(pty_test_child pty_test_child.cpp)
        target_compile_features(pty_test_child PRIVATE cxx_std_17)
        add_executable(ptyprocess_test ptyprocess_test.cpp mls-unit-test/test_main.cpp)
        target_link_libraries(ptyprocess_test PRIVATE matterm::pty)
        target_compile_definitions(ptyprocess_test PRIVATE PTY_TEST_CHILD="$<TARGET_FILE:pty_test_child>")
        add_dependencies(ptyprocess_test pty_test_child)
        add_test(NAME pty_process COMMAND ptyprocess_test)
        set_tests_properties(pty_process PROPERTIES TIMEOUT 20)
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

    set(TEST_FONT_FILE "" CACHE FILEPATH "TrueType font for atlas and rendering tests")
    if(NOT TEST_FONT_FILE AND MATTERM_ASSET_JetBrainsMono-Regular.ttf)
        set(TEST_FONT_FILE "${MATTERM_ASSET_JetBrainsMono-Regular.ttf}")
    endif()
    if(TEST_FONT_FILE)
        if(NOT EXISTS "${TEST_FONT_FILE}")
            message(FATAL_ERROR "TEST_FONT_FILE does not exist: ${TEST_FONT_FILE}")
        endif()
        add_test(NAME glyph_atlas COMMAND glyphatlas_test)
        set_tests_properties(glyph_atlas PROPERTIES
            ENVIRONMENT "GLYPH_TEST_FONT=${TEST_FONT_FILE}")
        add_test(NAME sdl_backend COMMAND sdlbackend_test)
        set_tests_properties(sdl_backend PROPERTIES
            ENVIRONMENT "GLYPH_TEST_FONT=${TEST_FONT_FILE};SDL_VIDEODRIVER=dummy")
        if(TARGET matterm-demo)
            add_test(NAME glyph_render_smoke
                COMMAND matterm-demo "${TEST_FONT_FILE}" --smoke-test)
            set_tests_properties(glyph_render_smoke PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=dummy")
            add_test(NAME terminal_model_render_smoke
                COMMAND matterm-terminal-demo "${TEST_FONT_FILE}" --smoke-test)
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
                add_test(NAME pty_frontend_closes_on_exit
                    COMMAND matterm "${TEST_FONT_FILE}" -- /bin/sh -c "exit 0")
                set_tests_properties(pty_frontend_closes_on_exit PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 5)
                add_test(NAME pty_frontend_advertises_truecolor
                    COMMAND matterm "${TEST_FONT_FILE}" --term xterm-256color -- /bin/sh -c
                        "test \"$COLORTERM\" = truecolor && test \"$TERM\" = xterm-256color")
                set_tests_properties(pty_frontend_advertises_truecolor PROPERTIES
                    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;COLORTERM=stale" TIMEOUT 5)
            endif()
        endif()
        if(MATTERM_BUNDLE_FONT)
            foreach(app IN ITEMS matterm matterm-demo matterm-terminal-demo)
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
