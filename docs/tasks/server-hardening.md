# Task: server hardening of the rate limiter and sessions

Assignment for an independent agent in its own worktree and branch. Read first:
Agents.md, docs/PARALLEL_WORK.md (task contract, handoff, environment notes),
docs/WEB_TERMINAL.md section 5 (threat model, rate limiting), docs/SERVER.md,
and the "Review follow-ups" in docs/tasks/server-skeleton.md, which this task
carries out. Code: `src/server/ratelimiter.*`, `src/server/auth.*`, their tests
`test/serverratelimiter_test.cpp` and `test/serverauth_test.cpp`.

## Goal

Close the four review follow-ups and the three unreviewed areas, with tests that
fail before and pass after.

## Scope

1. **Prune the source table.** `RateLimiter` removes a source only on success.
   At `maxSources` every new source gets 3600 s, so a scan from many addresses
   locks the owner out. Remove entries that have no active lockout and have been
   idle for a while (a config value, default 1 hour), at attempt time and
   amortized (no full scan on every call). Never evict an active lockout. Test
   the saturation scenario with the fake clock.
2. **Global budget as a lockout lever.** Decide with the reasoning in
   WEB_TERMINAL.md 5.3 and implement one option: count only failures against the
   global budget, or keep counting attempts but add `--reset-lockout`-style
   `RateLimiter::reset()` wiring on a signal or CLI flag. State the choice, the
   reason and the residual risk in docs/SERVER.md. Keep it configurable.
3. **IPv6 source key.** Add a pure function that turns a remote address into the
   limiter key (IPv4 as is, IPv6 as the /64 prefix, IPv4-mapped IPv6 as the
   IPv4 address) and use it where `server.cpp` builds `source_`. Unit test it.
4. **Threading contract.** Document in the headers that `SessionStore` and
   `RateLimiter` are used only from the server's single `io_context` thread, and
   add a debug assertion (thread id captured on first use) if cheap.
5. **Review, with tests for what you find:** server shutdown with active
   connections (`Server` destruction, `io_context::stop`), bounded write and
   close deadlines (a client that never reads must not hold a connection and
   its PTY forever: the WebSocket timeout is set, check it really fires and
   that the host cleans up), and credential parser edge cases in `auth.cpp`
   (`parsePassword`, `sessionCookie`: surrogates, NUL, duplicate cookies,
   oversize, trailing bytes).

## Out of scope

Config files (another agent), TLS, persistence of lockout state, the browser
client, `src/protocol/`, `src/platform/`, anything outside `src/server/` and
`test/server*`, except a defaulted field in a config struct.

## Owned and shared paths

Owned: `src/server/ratelimiter.*`, `src/server/auth.*`, `test/serverratelimiter_test.cpp`,
`test/serverauth_test.cpp`, new `test/server*_test.cpp`. Shared with other work,
edit minimally and report: `src/server/server.cpp` (only the source-key use and
shutdown/deadline fixes), `src/server/ptysessionhost.*`, `docs/SERVER.md`.
Another agent edits `src/server/main.cpp` and adds `src/server/config*`:
do not touch those.

## Acceptance

- New tests for every item above; the full suite passes in the default and in
  the server-only configuration (see docs/SERVER.md for the commands).
- clang-format applied; no build products committed.
- Update the status section at the end of docs/tasks/server-skeleton.md.

## Handoff

As in docs/PARALLEL_WORK.md: full commit SHAs in dependency order, changed
files, commands run and results, limitations, deferred items. Do not push.
