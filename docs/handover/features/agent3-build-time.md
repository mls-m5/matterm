# Shorter builds

Written by agent3, 2026-11-23. Task from the integrator: one test runner library, precompiled headers
for Boost.Asio/Beast.

## Done: the test runner is compiled once

`test/mls-unit-test/test_main.cpp` was compiled into each of the 81 test executables. It is now the
static library `mls_unit_test_main` (test/CMakeLists.txt, inside the same `if(BUILD_TESTING)` that
checks that the submodule is there, so the "no submodule" error is unchanged) and every test links it.
No test code changed.

Clean build, server build (`-DMATTERM_BUILD_SERVER=ON`, everything incl. tests), 32-core machine:

| | before | after |
|---|---|---|
| `cmake --build -j8` | 58.0 s | 51.3 s (-12 %) |
| `cmake --build -j4` | 103.9 s | 91.9 s (-12 %) |

127 of 127 tests pass, no warnings. The web build (build-web, emscripten) does not build tests (the
change is inside `BUILD_TESTING`); emcc is not available here, so that is reasoning, not a run.

## Measured and not done: precompiled headers for Asio/Beast

Tried `target_precompile_headers` with `<boost/asio.hpp>` and the Beast headers for matterm-server-core
(reused by the daemon core), matterm-server (reused by 18 test programs), chatd-core, chatd (reused by its
tests and mattrix-chat). Findings:

- A precompiled header made for a library (-fPIC) cannot be reused by a program (-fPIE; the hardening
  option turns position independence on): gcc warns `-Winvalid-pch`. So a library and a program each
  need their own, and the tests reuse the program's.
- With that working and warning-free: `-j8` 49.7 / 51.1 s against 51.3 s without, `-j4` 89.2 s against
  91.9 s. Total compile CPU time was 344 s with and without it: the Asio/Beast files (server.cpp 12 s,
  chatserver.cpp 12 s, the 20 server and chat tests 6-8 s each, about half of all compile time) spend their
  time instantiating templates, not reading headers. A ~3 % gain is not worth the extra CMake (four
  precompiled headers, the PIC/PIE rule), so it was left out. The same reason makes forward-declaring
  `io_context` or replacing `boost::asio::ip::address` in `ratelimiter.h`/`proxysource.h` pointless.

## What would help more (not done)

The compile time is mostly in ~25 files that instantiate Beast/Asio (about 175 s of 345 s of CPU) and
128 test files with a median of 1.2 s. A compiler cache (ccache) helps every rebuild after the first
and is not installed here; `CMAKE_CXX_COMPILER_LAUNCHER=ccache` is all it takes. The test fixtures
(`serverfixture.h`, `chatdfixture.h`) are included by about 15 tests: moving their template-heavy code
into one library would cut the instantiation, but changes test code, which the task excluded.
