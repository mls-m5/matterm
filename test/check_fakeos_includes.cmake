# src/fakeos must stay free of I/O, clocks, processes and platform headers, so
# the pretend system can never touch the real one. Run with -DFAKEOS_DIR=...
file(GLOB sources "${FAKEOS_DIR}/*.h" "${FAKEOS_DIR}/*.cpp")
set(forbidden
    "<filesystem>" "<fstream>" "<cstdio>" "<stdio.h>" "<iostream>" "<thread>"
    "<mutex>" "<future>" "<unistd.h>" "<sys/" "<fcntl.h>" "<termios.h>"
    "<signal.h>" "<csignal>" "<dirent.h>" "<windows.h>" "<SDL" "<emscripten"
    "<ctime>" "<time.h>")
set(failed FALSE)
foreach(file ${sources})
    file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include")
    foreach(line ${lines})
        foreach(item ${forbidden})
            string(FIND "${line}" "${item}" at)
            if(NOT at EQUAL -1)
                message(SEND_ERROR "${file}: forbidden include ${line}")
                set(failed TRUE)
            endif()
        endforeach()
    endforeach()
    file(READ "${file}" text)
    foreach(call "std::system" "std::chrono::system_clock" "steady_clock"
            "high_resolution_clock" "std::rand" "std::random_device" "getenv")
        string(FIND "${text}" "${call}" at)
        if(NOT at EQUAL -1)
            message(SEND_ERROR "${file}: forbidden use of ${call}")
            set(failed TRUE)
        endif()
    endforeach()
endforeach()
if(failed)
    message(FATAL_ERROR "fakeos include rule violated")
endif()
