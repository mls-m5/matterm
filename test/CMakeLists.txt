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

    set(TEST_FONT_FILE "" CACHE FILEPATH "TrueType font for atlas and rendering tests")
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
    else()
        message(STATUS "Set TEST_FONT_FILE to enable font-dependent atlas and rendering tests")
    endif()
endif()
