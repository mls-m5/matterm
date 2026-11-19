# Task: fix the fast-json parser bugs

Assignment for an independent agent in its own worktree (`.tmp/agent2`, branch
`agent2-fastjson`). Read first: Agents.md, docs/PARALLEL_WORK.md (task contract,
handoff, environment notes, submodule setup), `lib/fast-json/README.md`, the
library's tests in `lib/fast-json/test/`, and `src/server/config.cpp` (the strict
syntax check near the top is a workaround for the bugs below).

fast-json is the owner's own library and lives in the git submodule
`lib/fast-json` (remote `git@github.com:mls-m5/fast-json.git`). The fixes are
commits **inside the submodule**; the main repository then only moves the
submodule pointer.

## Goal

Make the fast-json parser reject malformed input and read numbers correctly, with
regression tests, then remove the workaround from the server config loader.

## Bugs (found by the config loader; each needs a failing test first)

1. The tokenizer never returns on a negative number such as `-5` (infinite loop).
   Negative numbers must parse: `-5`, `-0`, `-1.5e-3`.
2. `1.5` is read as `1`. Fractions and exponents must be parsed (`1.5`, `2e3`,
   `1E-2`). Decide how the library stores numbers today (integer or floating) and
   keep existing behavior for integers; document the decision in the library's
   README.
3. Truncated input is accepted: a file that ends inside an object, array, string
   or after a comma must be an error, not a partial value.
4. A missing comma between members or elements (`{"a":1 "b":2}`, `[1 2]`) is
   accepted. It must be an error.

Decision by the owner: a trailing comma before `]` or `}` may stay accepted.

Report errors the way the library already does (exception or error value);
if it has no error position, adding line and column is welcome but not required.
Do not change the public API of working inputs; run all existing library tests.

## Scope

1. In `lib/fast-json` (work on a branch inside the submodule, for example
   `fix-parser`): fix the four bugs, add regression tests to its test suite,
   plus a small mutation or fuzz style test (truncate every valid sample at every
   byte; every truncation that is not itself valid JSON must be an error and
   must terminate). Commit in small steps in the submodule.
2. In the main repository: advance the submodule pointer in its own commit, then
   remove the strict syntax check workaround from `src/server/config.cpp` (keep
   the error messages with line and column that `test/serverconfig_test.cpp`
   expects: if fast-json cannot report positions, keep the workaround only for
   position reporting and say so in the handoff). The server configuration tests
   must still pass.
3. Update the roadmap item for fast-json in ROADMAP.md (check it off).

## Out of scope

New fast-json features, serialization changes, the `JsonOut` writer, performance
work, anything outside `lib/fast-json`, `src/server/config.*` and
`test/serverconfig_test.cpp`.

## Environment

The worktree's submodules are not initialised. Set them up locally as described
in docs/PARALLEL_WORK.md (`lib/fast-json` and `test/mls-unit-test`). The
submodule's remote is `git@github.com`, which cannot be reached from the
sandbox: commit locally and do not push. Do not touch other submodules or
anything the owner has staged in the main checkout.

## Acceptance

- New tests fail before the fix and pass after; the library's own tests pass.
- Full suite of the main repository passes with `-DMATTERM_BUILD_SERVER=ON`.
- clang-format applied to project C++ (not to the library unless it has its own
  config); no build products committed.

## Handoff

As in docs/PARALLEL_WORK.md. Because the work spans two repositories, give the
submodule commits (full SHAs, in order, in `lib/fast-json`) and the main
repository commits separately. The integrator fetches the submodule commits from
your worktree's submodule and pushes them through the host tool before the
pointer. Do not push.
