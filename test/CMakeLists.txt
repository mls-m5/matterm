if(BUILD_TESTING)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mls-unit-test/test_main.cpp")
        message(FATAL_ERROR "Run git submodule update --init to fetch mls-unit-test, or configure with -DBUILD_TESTING=OFF.")
    endif()

    add_executable(matrixrenderer_test
        matrixrenderer_test.cpp
        mls-unit-test/test_main.cpp
    )
    target_compile_features(matrixrenderer_test PRIVATE cxx_std_17)
    target_include_directories(matrixrenderer_test PRIVATE ../src)
    add_test(NAME matrixrenderer_cell_grid COMMAND matrixrenderer_test)
endif()
