# Task: fuzz and property tests for the parsers and the protocol

Assignment for an independent agent in its own worktree (`.tmp/agent2`, new
branch `agent2-fuzz` from `main`). Read first: Agents.md, docs/PARALLEL_WORK.md,
ONGOING.md, docs/PROTOCOL.md, docs/VT.md, the ROADMAP.md item "Golden
transcripts from real programs, fuzz/property tests, and compatibility fixtures".

## Goal

Find crashes, hangs, unbounded growth and invariant violations in the code that
parses untrusted bytes, using deterministic tests that run in the normal suite
(seeded pseudo-random, a few seconds at most), and fix what they find.

## Targets (each needs tests; fix bugs in a separate commit from the tests)

1. `protocol::decodeClient` / `decodeServer` (`src/protocol/message.*`): random
   and mutated valid messages never crash, never read out of bounds, and every
   accepted message re-encodes to the same bytes (round trip).
2. `ServerSession` (`src/protocol/serversession.*`) driven by random sequences
   of client messages, ticks and host events with a fake host and clock:
   invariants hold (output window never exceeded, state machine reaches Closed,
   no message after Closed, Exit at most once).
3. `ClientSession` against a random/hostile server message stream: no crash,
   bounded buffering, ends in Exited or Failed on protocol violations.
4. `VtParser` + `TerminalModel` (`src/terminal/vt/`, `src/terminal/`): random byte
   streams and mutated real-looking transcripts (SGR, CSI, OSC, DCS, partial
   UTF-8, huge parameters) never crash or hang, keep every model invariant
   (cursor inside the screen, row count, scrollback bound), and feeding the same
   bytes in different chunkings gives identical screens.
5. `parsePassword` and `sessionCookie` (`src/server/auth.*`) and the config
   loader (`src/server/config.*`): random and mutated inputs terminate and
   never accept something the strict rules reject.

## Rules

Seed from a fixed constant and print the seed and the failing input (hex) on
failure so it can be replayed. Keep each test under one second on a laptop. No
new dependencies. If a real bug needs a design decision, write the failing test
(marked as expected-fail with a comment) and describe it in the handoff.

## Out of scope

Features, refactors, the renderer, the web client, the server's networking.

## Owned and shared paths

Owned: new `test/*fuzz*_test.cpp` or `test/*property*_test.cpp` files. Shared,
edit minimally and report: `test/CMakeLists.txt` and the source of any bug you
fix. Another agent edits `src/frontend/` and the mouse code of
`app/ptyfrontend.cpp` and `web/main.cpp`.

## Acceptance

Default and `-DMATTERM_BUILD_SERVER=ON` suites pass; each fixed bug has a test
that failed before; clang-format applied; no build products committed. Update
the ROADMAP item (check it off only for what is really covered).

## Handoff

As in docs/PARALLEL_WORK.md: SHAs in order (tests first, fixes separate), commands
run and results, bugs found, limitations. Do not push.
