# Task: matterm-server, first vertical slice (phase 2, part 1)

Assignment for an independent agent working in its own git worktree and branch.
Read first: Agents.md, docs/PARALLEL_WORK.md (task contract, handoff, sandbox
notes), docs/WEB_TERMINAL.md (design and threat model), docs/PROTOCOL.md (wire
format), then `src/protocol/` (codec and `ServerSession`, already implemented)
and `src/platform/posix/ptyprocess.h`.

## Goal

A loopback-only `matterm-server` executable that a test client can log in to and
use to run a shell over the matterm-tty protocol. No browser client, no config
file, no TLS. This is the server track of the dependency graph in the chat that
produced this document; another session works on the browser track in parallel.

## Base and branch

Branch `agent2-server` in the worktree `.tmp/agent2`, based on the commit the
branch was created from. Rebase onto `main` only if the integrator asks.

## Scope

1. **Build system.** Add `MATTERM_BUILD_SERVER` (default OFF). The server target
   is C++20 and links Boost (Beast/Asio headers, `Threads`), libsodium, and
   `matterm::protocol` + `matterm::pty`. It must not need SDL2, SDL2_ttf, fonts or
   Fontconfig: make the unconditional `find_package(SDL2 ...)` in the root
   CMakeLists.txt conditional on the targets that need it (app, demos, renderer,
   and the SDL-based tests), without changing the default build's behavior or test
   list. Verify both: the default configuration and
   `-DMATTERM_BUILD_APP=OFF -DMATTERM_BUILD_DEMO=OFF -DMATTERM_BUILD_SERVER=ON`.
   Keep the root diff small; put server rules in `src/server/CMakeLists.txt` if
   practical.
2. **`PtySessionHost`** (`src/server/`): implements `protocol::SessionHost` on
   top of `PtyProcess`. Read from the PTY only up to `ServerSession::outputBudget()`
   per pump; queue PTY input (limit 1 MiB, `writeInput` returns false beyond it);
   report process exit through `onProcessExit` only after the PTY output has been
   drained; `terminateProcess` = SIGHUP, then SIGKILL after a grace period.
   `PtyProcess::resize` has no pixel size and its environment handling is fixed:
   you may extend it compatibly (extra defaulted parameters, an optional
   environment allow-list) but say so in the handoff, because the native app uses
   it too. The program and its arguments come from server configuration (a
   command-line option for now), never from the client.
3. **HTTP and WebSocket** (Boost.Beast, one `io_context`, single thread is fine):
   - Listen on `127.0.0.1` only. Refuse any other address unless
     `--allow-plain-http` is given (print a warning); refuse to run as root.
   - `GET /` serves a small placeholder page (static string is fine).
   - `POST /login` with a JSON body `{"password": "..."}` (a hand-written
     minimal parser is fine for this one field; the fast-json configuration
     layer comes later).
   - `GET /ws`: WebSocket upgrade requiring a valid session cookie, an `Origin`
     equal to the configured allowed origin, and subprotocol `matterm-tty.v1`.
     Binary messages only; max message 16384 bytes; no `permessage-deflate`.
     Pass each message to `ServerSession::onMessage`; run `tick()` by
     `nextDeadline()`. One session per login to start with.
   - Unauthenticated connections: small cap and a 5 second deadline.
4. **Authentication** (libsodium): Argon2id password verification
   (`crypto_pwhash_str_verify`), a dummy hash when none is configured, minimum
   password length 24 in `--set-password` (reads stdin, never argv, writes the hash
   file with mode 0600). Session tokens: 32 random bytes, only their hash is kept
   server-side, cookie `matterm_session` with `HttpOnly; Secure; SameSite=Strict;
   Path=/`, idle and absolute timeouts (constants for now, configurable struct).
   Identical response for every login failure.
5. **Rate limiter** (`src/server/ratelimiter.h/.cpp`, independent and
   unit-tested with a fake clock): per-source exponential lockout and a global
   hourly attempt budget exactly as in docs/WEB_TERMINAL.md section 5.3, all values
   in a plain config struct (loading it from a file is the next slice). Loopback
   exempt from the global budget by default. State persistence across restarts is
   optional in this slice; design the interface so it can be added.

## Out of scope (do not start)

Configuration files and fast-json, TLS, second factor, session resume, the
browser client, Podman files, changes to `src/protocol/` semantics, `src/terminal/`,
`src/render/`, `app/`. If the protocol needs a change, stop and describe it in the
handoff instead.

## Owned and shared paths

Owned: `src/server/**`, `test/server*_test.cpp`, `docs/SERVER.md` (new: how to
build, set a password, run, and test it). Shared, edit minimally and report:
root `CMakeLists.txt`, `test/CMakeLists.txt`, `src/platform/posix/ptyprocess.*`,
`.matterm.files` (local, untracked). Do not touch ROADMAP.md except to check an
item you actually finished.

## Security requirements (acceptance, not nice-to-have)

- Never log passwords, tokens, or terminal data; log auth events only.
- Constant-time comparisons for secrets; no secrets in URLs.
- Strict `Origin` check on login and upgrade; `SameSite=Strict`, `HttpOnly` cookie.
- Bounded everything: request size, header size, body size (`POST /login` at most
  a few hundred bytes), connections, pending input, message size.
- The rate limiter runs before password hashing.
- Listening beyond loopback is impossible without an explicit flag.

## Acceptance tests

- `ratelimiter_test` with a fake clock (backoff schedule, reset on success,
  global budget, loopback exemption, `Retry-After` value).
- Session store/auth tests (wrong password, expired cookie, forged cookie).
- An integration test: start the server on an ephemeral loopback port, log in with
  a Beast client, upgrade, `Hello`, run `echo hello` in a shell started with a
  fixed argument list, see the output, `Resize`, see `Exit` with the right code,
  and check the process is gone. Also: upgrade without cookie is refused, wrong
  `Origin` is refused, a message over 16 KiB closes the connection.
- The whole existing suite still passes in the default configuration, and the
  server-only configuration builds and passes its tests without SDL2 installed
  (verify by configuring with the SDL options off).

## Handoff

Follow docs/PARALLEL_WORK.md: full commit SHAs in dependency order, changed
files, API changes (especially to `PtyProcess`), the commands you ran and their
results, known limitations, and anything you deferred. Do not push; the
integrator pushes through the host tool. Commit in small steps, with
clang-format applied and no build products.
