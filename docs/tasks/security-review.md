# Task: security review of the server and the browser client

Assignment for an independent agent in its own worktree (`.tmp/agent2`, new
branch `agent2-security` from `main`). Read first: Agents.md,
docs/PARALLEL_WORK.md, docs/WEB_TERMINAL.md (threat model, sections 5 and 6),
docs/SERVER.md, docs/SERVER_CONFIG.md, docs/PROTOCOL.md, `deploy/` (nginx example,
Containerfile, compose), and the code: `src/server/**`, `src/protocol/**`,
`web/login.js`, `web/shell.html`, `web/main.cpp`, `web/websocketchannel.*`.

## Context

The server will be reachable from the internet behind an nginx that terminates
TLS, and gives a shell to whoever logs in. One owner, one long password. The
review should assume a patient remote attacker who can send arbitrary bytes, and a
malicious web page open in the owner's browser at the same time.

## Goal

Find and fix real weaknesses; for each finding write a test that fails before the
fix. Be adversarial, not a checklist.

## Areas

1. **Authentication and sessions**: login rate limiting before hashing, timing
   differences (unknown vs wrong password, configured vs not), cookie flags and
   scope, session fixation and rotation, the single-upgrade claim, token entropy
   and comparison, behavior behind a reverse proxy (client address, trusted proxy
   handling if present), lockout bypass via many sources, headers or connection
   reuse.
2. **Cross-site and browser**: Origin and Host checks (case, port, trailing dots,
   duplicate headers, `null` origin), CSWSH (WebSocket from a hostile page), CSRF
   on `/login`, CSP (the actual headers served for static files and API
   responses; does anything allow inline script or framing), `X-Content-Type-
   Options`, caching of authenticated responses, `window.open`/link policy in
   code that exists now, clipboard paste handling (bracketed paste escape
   sequences injected through the clipboard text), OSC 52 and title sequences
   from the program toward the browser (must not become script or clipboard
   access).
3. **HTTP and WebSocket handling**: request smuggling or pipelining leftovers (the
   code rejects buffered early data; check it), slow-loris and deadline
   coverage on every phase, message size and count limits, fragmented messages,
   close handshake abuse, static file serving (names, encoding, `..`, absolute
   paths, symlinks, case folding, very large directories), error messages that
   leak paths or internals.
4. **Process and host**: what the shell inherits (environment allow-list,
   descriptors, resource limits), cleanup of process groups, behavior when the
   PTY floods, the root refusal, file permission checks on credentials and config
   (races, symlinks), anything that logs secrets or terminal data.
5. **Deployment files**: `deploy/` nginx example (TLS settings, `proxy_read_timeout`,
   WebSocket upgrade, forwarded headers that the server does or does not trust,
   security headers, rate limiting at the proxy), Containerfile and compose (user,
   capabilities, read-only paths, network). Check they match what the code
   assumes; list what was not verified because containers cannot run here.

## Output

- `docs/SECURITY_REVIEW.md`: findings ranked by severity (what, how to exploit,
  the fix or why it is acceptable), what was checked and found fine, and what was
  not checked. No secrets in the document.
- Fixes in separate commits from the tests that expose them. For a finding that
  needs a design decision (not a clear bug), write the failing test as an
  expected failure with a comment, describe the options in the review, and do not
  guess.

## Out of scope

New features, a rewrite of the transport, TLS inside the server, second factor,
session resume (all planned separately); the native app and the renderer.

## Owned and shared paths

Owned: `docs/SECURITY_REVIEW.md`, new `test/*security*_test.cpp`. Shared, edit
minimally and report: `src/server/**`, `web/login.js`, `web/shell.html`,
`deploy/**`, `test/CMakeLists.txt`. Another agent edits mouse and link handling in
`app/ptyfrontend.cpp`, `web/main.cpp` and `src/frontend/`: do not touch those.

## Acceptance

Default and `-DMATTERM_BUILD_SERVER=ON` suites pass; every fixed finding has a test
that failed before; clang-format applied; no build products committed.

## Handoff

As in docs/PARALLEL_WORK.md: SHAs in order (tests, then fixes), commands run,
findings summary with severities, limitations. Do not push.
