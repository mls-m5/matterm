string(RANDOM LENGTH 12 suffix)
set(capture "${CMAKE_CURRENT_BINARY_DIR}/matterm-record-test-${suffix}.vt")
set(payload [=[printf '\033[48;2;37;65;49m \342\240\213\033[0m\000']=])

execute_process(COMMAND "${APP}" "${FONT}" --record-output "${capture}" -- /bin/sh -c "${payload}"
    RESULT_VARIABLE status ERROR_VARIABLE diagnostic TIMEOUT 5)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Recording failed (${status}): ${diagnostic}")
endif()
file(READ "${capture}" recorded HEX)
if(NOT recorded STREQUAL "1b5b34383b323b33373b36353b34396d20e2a08b1b5b306d00")
    message(FATAL_ERROR "Unexpected PTY capture bytes: ${recorded}")
endif()

execute_process(COMMAND "${APP}" "${FONT}" --record-output "${capture}" -- /bin/sh -c "printf overwritten"
    RESULT_VARIABLE status ERROR_VARIABLE diagnostic TIMEOUT 5)
file(READ "${capture}" after HEX)
file(REMOVE "${capture}")
if(NOT status STREQUAL "1" OR NOT diagnostic MATCHES "Could not create PTY recording" OR NOT after STREQUAL recorded)
    message(FATAL_ERROR "Recorder did not reject an existing file: ${status}: ${diagnostic}")
endif()
