# Warnings, hardening and clang-tidy

## Compiler flags (`cmake/Warnings.cmake`)

Applied to all native targets (not the Emscripten build), on by default:

- `MATTERM_WARNINGS` (ON): `-Wall -Wextra -Wpedantic -Wformat=2 -Wformat-security
  -Wformat-signedness -Wshadow -Wvla -Wcast-qual -Wcast-align -Wnon-virtual-dtor
  -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
  -Wundef` and, with GCC, `-Wduplicated-cond -Wduplicated-branches -Wstack-protector`.
  `-Wmissing-field-initializers` is off (aggregate event structs are filled partially
  on purpose).
- `MATTERM_WERROR` (OFF): warnings become errors. The tree builds warning-free, so CI
  can turn this on.
- `MATTERM_HARDENING` (ON): `-fstack-protector-strong`, `_FORTIFY_SOURCE=2` (non-Debug),
  `_GLIBCXX_ASSERTIONS`, `-fno-strict-overflow`, `-fno-delete-null-pointer-checks`,
  PIE and `-Wl,-z,relro,-z,now,-z,noexecstack`.
- `MATTERM_SANITIZE` (OFF): AddressSanitizer + UndefinedBehaviorSanitizer for test runs:
  `cmake -S . -B build-asan -DMATTERM_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`.

The C++ standard is set to 17 globally so that clang-tidy and GCC parse the same language.

## clang-tidy (`.clang-tidy`, `tools/clang-tidy.sh`)

```sh
cmake -S . -B build          # exports compile_commands.json
tools/clang-tidy.sh build -j8
```

A full run takes a few minutes (about 2.5 min with 8 jobs), so it is not part of the
normal build or of every change. Run it every so often, after a batch of changes, and
use `tools/clang-tidy.sh build --changed` (only the .cpp files that differ from
`origin/main`) for a quick check on one task.

Enabled: `bugprone-*`, `cert-*`, `clang-analyzer-{core,security,unix,cplusplus}`,
`concurrency-*`, a few `misc-*`, `cppcoreguidelines-narrowing-conversions` (not for
int/float) and `-slicing`. Disabled as style noise: init-variables, member-init,
branch-clone, mt-unsafe libc calls, integer-to-float narrowing, the seeding checks (the
demo RNG is deterministic on purpose).

Findings fixed when this was added: an untrusted-input `atoi` in the demo's mouse
report parser (overflow, now clamped `strtol`), a signed-char array index, a
size_t/iterator narrowing in the server config, missing `<optional>`/`<string_view>`
includes, and the shadowing and sign-compare warnings. One clang-analyzer false positive
(`InnerPointer` on the password zeroing in `server.cpp`) is silenced with a
`NOLINTNEXTLINE` and a comment.
