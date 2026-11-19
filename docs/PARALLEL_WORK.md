# Independent Agent Work and Integration

## Starting Point

Use a separate clone/worktree and branch for each task, based on an agreed full
commit SHA. Never have multiple agents edit the same working tree. The operator
starts independent agents; this document does not launch any work automatically.

Before starting: read Agents.md, DESIGN.md, ROADMAP.md and the task's subsystem
notes. Initialize submodules with `git submodule update --init --recursive`.
Currently matperf is at repository root (`matperf/`), not under lib/. Its URL
uses SSH, so other clones need suitable access or a local URL override. Do not
rewrite or advance submodule pins as part of unrelated tasks.

## Task Contract

Each assignment should specify:

- Base commit, branch, goal, owned paths, and forbidden/shared files.
- Public API assumptions and acceptance tests, including build configurations.
- Whether implementation, independent tests, or review is requested.
- A small commit series with no unrelated formatting or generated build files.

Shared contracts (FChar layout, RenderFrame, TerminalModel, parser events) need
agreement before concurrent implementation. Do not independently redesign them.
Prefer adding new files rather than having multiple agents edit root CMakeLists,
ROADMAP.md, or the same test registration file. Supply an integration snippet if
those shared files are outside the task's ownership.

## Handoff

Return full commit SHAs, changed files, API changes, commands/results, limitations,
and any dependency commits. Main integrator inspects the diff, fetches the task
branch if needed, and cherry-picks the commits in dependency order. Never copy an
entire repository over the integration tree or force/reset unrelated changes.
Resolve conflicts explicitly, update the local .matterm.files list, format project
C++, and run the whole test suite before accepting the work. Keep both demos.

## Candidate Parallel Tasks

After the color contract lands:

1. **Pure benchmark fixtures:** deterministic prebuilt ASCII/CSI workloads and
   portable stdout benchmark, under bench/. No parser/model changes. Report
   write completion separately from a bounded DSR reply barrier. Do not claim
   either measures physical display completion.
2. **Configuration layer:** versioned text schema, XDG path resolution, validation,
   atomic saving, and CPU-only tests. Depends on the agreed Palette API; no GUI.
3. **Independent color tests/review:** layout/default/indexed/RGB, inverse/faint,
   old cells after theme changes. Do not change production APIs without discussion.
4. **Scroll bug investigation:** reproduce empty/leading-space behavior in both
   modes and propose narrowly scoped regression tests before changing policy.
5. **Profiling review:** inspect matperf integration requirements/concurrency and
   coarse instrumentation points. Do not add tracing inside per-cell loops.

A first trial should use one bounded task (for example benchmark fixtures) and
review its integration cost before expanding the fanout. Large terminal semantic
changes should not run in parallel with changes to the same model/parser API.

## Active Tasks

Agents work in separate worktrees with non-overlapping owned paths. Finished and
integrated: server skeleton, hardening, configuration, ScriptedLink, fast-json
fixes, resize duplication, zero-width glyph.

- `docs/tasks/window-frame.md`: framed, narrower terminal window with margins,
  rounded corners and shadow. Worktree `.tmp/agent3`, branch `agent3-frame`.

- `docs/tasks/server-sessions.md`: `trusted_proxy`, then server-owned sessions
  with an optional replay ring (phase 1 of docs/MULTI_TERMINAL.md). Worktree
  `.tmp/agent2`, branch `agent2-sessions`.

Integrated since the previous list: security review, URL opening,
touch gestures (`docs/tasks/touch-gestures.md`), empty-scroll/grow/ED3 work
(`docs/tasks/empty-scroll-and-reflow.md`; reflow is only designed, see
docs/SCROLLING_AND_REFLOW.md).

Agents can be reached through the MCP message tools (`list_peers`,
`send_message`, `receive_messages`); the integrator is the project named by
`list_peers`.

The integrator owns the browser client in `web/` and integration. Integrate by
cherry-picking in dependency order and running the full suite.

## Environment Notes for Agents

Facts learned while working in the agent sandbox; check them rather than
rediscovering them.

- The sandbox cannot run containers (Podman installs but cannot mount `/proc`
  in a nested PID namespace), and there is no `ssh` binary, so submodules with
  `git@github.com` URLs cannot be fetched or pushed from here. Pushes go through
  the host tool, one approved push at a time; agents on a task branch do not push.
- Installed for the web work: Boost headers (including Beast/Asio) and libsodium.
  Missing tools must be requested from the operator. There is no Emscripten.
- A new worktree has uninitialized submodules. For a local setup, point them at
  the main checkout instead of the network, for example:
  `git config submodule.test/mls-unit-test.url <main-checkout>/test/mls-unit-test`
  followed by `git -c protocol.file.allow=always submodule update --init
  test/mls-unit-test`. Only the unit-test framework is needed for tests.
- `.matterm.files` is untracked on purpose (`*.files` is ignored); update it
  locally. Run clang-format on project C++ before committing.
- mls-unit-test: `EXPECT` returns `void`, so it cannot be used in helper
  functions that return a value; return a sentinel and compare in the test.
- podman-compose 1.0.3 ignores `name:` (use `-p`) and rejects nested `${A:-${B}}`.
- Never advance submodule pointers or touch a user's staged submodule changes as
  part of an unrelated task.
