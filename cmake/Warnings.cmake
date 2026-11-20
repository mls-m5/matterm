# Warning and hardening flags for matterm's own code. Included from the top-level
# CMakeLists.txt before any target is created. Options:
#   MATTERM_WARNINGS  (ON)   extra warnings, focused on memory and format safety
#   MATTERM_WERROR    (OFF)  turn warnings into errors (CI)
#   MATTERM_HARDENING (ON)   stack protector, FORTIFY_SOURCE, PIE, relro (native only)
#   MATTERM_SANITIZE  (OFF)  address+undefined sanitizers (debug runs)
option(MATTERM_WARNINGS "Enable the extra compiler warnings" ON)
option(MATTERM_WERROR "Treat warnings as errors" OFF)
option(MATTERM_HARDENING "Enable compiler and linker hardening flags (native builds)" ON)
option(MATTERM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(MATTERM_WARNINGS)
        add_compile_options(
            -Wall -Wextra -Wpedantic
            -Wformat=2 -Wformat-security -Wformat-signedness
            -Wshadow -Wvla -Wcast-qual -Wcast-align
            -Wnon-virtual-dtor -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion
            -Wimplicit-fallthrough -Wundef
            -Wno-missing-field-initializers)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-Wduplicated-cond -Wduplicated-branches
                -Wstack-protector)
        endif()
        if(MATTERM_WERROR)
            add_compile_options(-Werror)
        endif()
    endif()
    if(MATTERM_HARDENING)
        add_compile_options(-fstack-protector-strong -fno-delete-null-pointer-checks
            -fno-strict-overflow)
        add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
        add_compile_definitions(_GLIBCXX_ASSERTIONS)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    endif()
    if(MATTERM_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
