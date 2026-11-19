# Task: trusted_proxy, then server-owned sessions with optional replay (phase 1)

Assignment for an independent agent in its own worktree (`.tmp/agent2`, new branch
`agent2-sessions` from `main`). Read first: Agents.md (how we work),
docs/PARALLEL_WORK.md, **docs/MULTI_TERMINAL.md (the plan; sections 3, 4, 8, 9 and
10 are the contract)**, docs/PROTOCOL.md (sections 7 and 8, planned resume),
docs/SERVER.md, docs/SERVER_CONFIG.md, docs/SECURITY_REVIEW.md, docs/WEB_TERMINAL.md
(`trusted_proxy`), and the code in `src/server/**`, `src/protocol/**`,
`src/transport/clientsession.*`, `test/server*_test.cpp`, `test/serversecurity_test.cpp`.

## Steps (commit each separately, tests first)

1. **`trusted_proxy` (small, first).** Implement the design in
   docs/WEB_TERMINAL.md ("trusted_proxy", "trusted_proxy_hops"): the client address
   for the rate limiter and logs comes from `X-Forwarded-For` only when the peer
   is the configured proxy; otherwise the peer address is used. Enable the
   disabled test in `test/serversecurity_test.cpp`, add tests for spoofed headers
   from an untrusted peer, multiple entries, hops, IPv6 (/64 key), malformed values,
   and config validation (`trusted_proxy = any` only when documented as safe).
   Update docs/SERVER_CONFIG.md, docs/SECURITY_REVIEW.md (finding resolved) and the
   deploy files so they pass the flags the code really accepts.
2. **Protocol design note first**: write the resume fields (`sessionId`,
   `resumeOffset`, `Welcome.flags`, the `features` bit, attach/new in the upgrade
   request, take-over close reason) in docs/PROTOCOL.md and get the layouts
   unambiguous with worked examples. Update the protocol tests and fuzz inputs.
3. **SessionRegistry** in the server per docs/MULTI_TERMINAL.md section 3: sessions
   outlive connections when `persistent_sessions` is on; random 128-bit ids; one
   attached connection at a time with **take-over** (the old one is closed with a
   reason); `max_sessions`, `detached_timeout_seconds`; a shell that exits ends its
   session and the client is told; several WebSocket upgrades per login with a
   configurable limit (re-check `SessionStore` and the rate limiter rules).
4. **Replay ring** per section 4: `(timestamp, bytes)` chunks, per-session and total
   caps, oldest dropped first, `replay_buffer_bytes = 0` means no buffer and no
   persistence (sessions end with their connection, as today), resize + SIGWINCH
   on reattach, safe start of the replay (newline-aligned or dropped bytes).
   Memory is wiped on session end; nothing is logged.
5. **Tests**: dropped connection and reattach, take-over, timeout, ring wrap,
   memory caps, shell exit while detached, zero-buffer mode, limits, unauthenticated
   attach attempts, guessed ids; extend the fuzz tests for the new fields.
6. **Docs**: docs/SERVER.md, docs/SERVER_CONFIG.md (new settings and defaults),
   docs/MULTI_TERMINAL.md (mark what is implemented, record decisions), ROADMAP.md.
   Add a short security re-review of the new attack surface to
   docs/SECURITY_REVIEW.md.

## Out of scope

The browser client (tabs, `web/`), recording and playback, panes, the settings UI,
the mode-scanner for private modes (only if the plan's "Open" item proves needed;
note it if replay shows the problem in tests), native app.

## Owned and shared paths

Owned: `src/server/**`, `src/protocol/**`, `src/transport/**`, their tests, `deploy/`,
server docs. Shared, edit minimally and report: `docs/PROTOCOL.md`,
`docs/SECURITY_REVIEW.md`, `ROADMAP.md`. Do not edit `web/` (the integrator and
agent3 work there); if the web client needs a change to keep working with the new
protocol, describe it in the report instead.

## Acceptance

Full ctest suite passes with `-DMATTERM_BUILD_SERVER=ON`; all fuzz tests pass; each
fix or feature has a test that failed or did not exist before; the existing web
client still works unchanged with persistence off (the integrator will run the
e2e scripts); a short report lists commits, settings added, and what could not be
verified. Do not push.
