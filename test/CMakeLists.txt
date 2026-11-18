if(BUILD_TESTING)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mls-unit-test/test_main.cpp")
        message(FATAL_ERROR "Run git submodule update --init to fetch mls-unit-test, or configure with -DBUILD_TESTING=OFF.")
    endif()

    add_executable(cellgridview_test
        cellgridview_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_compile_features(cellgridview_test PRIVATE cxx_std_17)
    target_include_directories(cellgridview_test PRIVATE ../src)
    add_test(NAME cell_grid_view COMMAND cellgridview_test)

    add_executable(glyphatlas_test
        glyphatlas_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_link_libraries(glyphatlas_test PRIVATE glyphatlas)

    set(TEST_FONT_FILE "" CACHE FILEPATH "TrueType font for atlas and rendering tests")
    if(TEST_FONT_FILE)
        if(NOT EXISTS "${TEST_FONT_FILE}")
            message(FATAL_ERROR "TEST_FONT_FILE does not exist: ${TEST_FONT_FILE}")
        endif()
        add_test(NAME glyph_atlas COMMAND glyphatlas_test)
        set_tests_properties(glyph_atlas PROPERTIES
            ENVIRONMENT "GLYPH_TEST_FONT=${TEST_FONT_FILE}")
        add_test(NAME glyph_render_smoke
            COMMAND 2026-terminal-renderer "${TEST_FONT_FILE}" --smoke-test)
        set_tests_properties(glyph_render_smoke PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    else()
        message(STATUS "Set TEST_FONT_FILE to enable font-dependent atlas and rendering tests")
    endif()
endif()
