# CLI help must work without a display, font assets, or a child process.
set(ENV{SDL_VIDEODRIVER} "matterm-invalid-test-driver")
foreach(args IN ITEMS "--help" "-h" "/missing/font.ttf;--help" "--smooth-cursor;--help")
    execute_process(COMMAND "${APP}" ${args}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
    if(NOT result EQUAL 0 OR NOT error STREQUAL "")
        message(FATAL_ERROR "Help failed for ${args}: ${result}: ${error}")
    endif()
    foreach(required IN ITEMS "Usage:" "--smooth-cursor" "native PTY only"
            "--font-size" "--record-output" "--input" "--showcase" "-- COMMAND"
            "no --smooth-scroll flag" "Example:")
        string(FIND "${output}" "${required}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Help for ${args} is missing: ${required}")
        endif()
    endforeach()
endforeach()

foreach(args IN ITEMS "--smooth-scroll" "--smooth-cursor;--showcase"
        "--smooth-cursor;--input;/missing/input" "--smooth-cursor;--smoke-test")
    execute_process(COMMAND "${APP}" ${args}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
    if(NOT result EQUAL 1 OR NOT output STREQUAL "")
        message(FATAL_ERROR "Invalid options were not rejected: ${args}: ${result}")
    endif()
    if(NOT error MATCHES "Usage:|--smooth-cursor requires")
        message(FATAL_ERROR "Missing CLI diagnostic for ${args}: ${error}")
    endif()
endforeach()
