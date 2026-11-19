# Web Terminal: Server, Protocol, and Security Design

Status: **agreed design, nothing implemented.** It refines the "Future
Direction: Emscripten and Browser Clients" section of ROADMAP.md. The decisions
made while reviewing it are collected in section 11; open items are marked
*Open*.

## 1. Goals and Non-Goals

Goals

- Run the matterm terminal in a browser against a shell on a server I control.
  The first target is my laptop, bound to loopback only; the later target is a
  public server.
- Keep the architecture `emulator (browser) <-> protocol <-> server <-> tty`.
  The browser owns parser, model, renderer, and key encoding; the server owns
  the process, the PTY, authentication, and policy.
- Make the transport replaceable (WebSocket first, WebRTC data channel later)
  and reuse the same browser-side libraries for a scripted "terminal simulator"
  with no server at all.
- Treat the server as a remote-shell service from day one: a flaw is remote
  code execution as the server user.

Non-goals (for now)

- Multi-user accounts, sharing/collaborative sessions, file transfer, or
  server-side terminal emulation (screen diffing, mosh-style).
- A general web framework. The server serves a handful of static files, one
  login endpoint, and the terminal socket.
- Mobile/IME polish and clipboard integration beyond basics (see section 6).

## 2. Architecture

```
  Browser (WASM)                         Server (native, Linux)
 +-------------------------+            +--------------------------------+
 | Renderer (SDL/WebGL)    |            | HTTP: static files, /login     |
 | TerminalModel + VtParser|            | Auth + rate limiter + sessions |
 | InputEncoder (keys,     |  protocol  | Session manager (ring buffer)  |
 |   mouse, paste, focus)  |<==========>| PtyProcess  -->  shell / tty   |
 | Transport (WebSocket)   |  WS / DC   |                                |
 +-------------------------+            +--------------------------------+
```

Key property: **the protocol carries the same bytes a native terminal exchanges
with the PTY.** Output frames are raw PTY output; input frames are exactly what
`InputEncoder` produces (including mouse, focus, bracketed paste, and parser
replies such as DA and OSC color reports). The server never parses VT, so it
needs no knowledge of terminal modes, and the native and web clients share every
byte-level behavior (and test).

Consequences of that choice:

- Terminal state lives only in the browser. After a disconnect the server cannot
  redraw the screen by itself; see reconnection (section 4.5).
- Resize is a separate control message (the PTY needs `TIOCSWINSZ`).
- The browser must be able to tell the server how many bytes it has consumed
  (flow control, section 4.4).

### 2.1 Code layout

| Path | Contents | Depends on |
|---|---|---|
| `src/protocol/` | Message types, binary encode/decode, limits. Pure C++17, no I/O, no SDL. | nothing |
| `src/transport/` | `MessageChannel`, `TerminalLink`, and `ClientSession` (done); the WebSocket channel for WASM and the scripted link (later). | protocol |
| `src/platform/posix/ptyprocess.*` | Already exists; the server reuses it unchanged. | - |
| `src/server/` | `matterm-server`: HTTP/WebSocket listener (Boost.Beast on Boost.Asio), auth, rate limiting, session manager, configuration. Linux only, off unless `MATTERM_BUILD_SERVER=ON`. | protocol, ptyprocess, Boost, libsodium, fast-json |
| `web/` | HTML shell, JS glue (WebSocket, clipboard, resize observer), CSP. | - |
| `app/web/` | Emscripten entry point: init / tick / shutdown, no blocking loop. | terminal, vt, renderer, transport |

`PtySession` currently bundles `PtyProcess`, `VtParser`, and `TerminalModel`.
The server must use only `PtyProcess`; no refactor is needed, but the web
client should not reuse `PtySession`.

Two small interfaces separate the layers (`src/transport/`):

- `MessageChannel`: a reliable, ordered, message-oriented channel (WebSocket,
  later a WebRTC data channel, or an in-memory pair in tests). `send`, `close`,
  and `opened` / `message` / `closed` events. It knows nothing about the protocol.
- `TerminalLink`: what a terminal front end needs from wherever its terminal
  lives: `sendInput`, `resize`, `close`, and `output` / `exited` / `closed`
  events. The model, parser, and renderer see only this.

Implementations of `TerminalLink`: `ClientSession` (implemented: speaks
matterm-tty over any `MessageChannel`, sends `Hello`, chunks input, acknowledges
parsed output, pings, and reports exit or failure), a future `PtyLink` for the
native application, and a `ScriptedLink` for the simulator.

## 3. Threat Model

Assets: command execution as the server user, the files that user can read, and
the credentials. Attackers considered: anyone who can reach the port (internet
scanners and password guessers), a malicious web page the owner visits (CSRF /
cross-site WebSocket hijacking), a network observer, and a compromised or
buggy client sending malformed frames. Out of scope: a compromised server user
account or host, and malware in the owner's browser.

| Threat | Mitigation |
|---|---|
| Online password guessing | Very long random passphrase, Argon2id verification, per-source and global rate limiting with persistent lockout (5.3) |
| Credential theft on the wire | TLS on any non-loopback interface; refuse to start otherwise (5.1) |
| Cross-site WebSocket hijacking / CSRF | Strict `Origin` check on login and upgrade, `SameSite=Strict` cookie, no credentials in URLs |
| Session theft | 256-bit random token, `HttpOnly; Secure; SameSite=Strict`, idle and absolute timeouts, rotate on login, revoke on logout |
| Memory-safety bugs in parsing | Small bounded binary protocol, size limits before allocation, fuzzing (8) |
| Resource exhaustion | Caps on connections, pending output, message size, sessions, and unauthenticated connection time (4.2, 4.4, 5.3) |
| Privilege escalation after login | Server runs as an unprivileged user, never root; client cannot choose the program; systemd/container hardening (5.4) |
| Log leakage | Never log passwords, tokens, or terminal data; log auth events with a hashed/truncated source |

## 4. Protocol ("matterm-tty", version 1)

### 4.1 Transport mapping

A reliable, ordered, message-framed channel. WebSocket binary messages map
directly (subprotocol `matterm-tty.v1`, negotiated via
`Sec-WebSocket-Protocol`). A WebRTC data channel created `ordered: true` with
default reliability can carry the same messages unchanged. Text frames are
rejected. No compression (`permessage-deflate` disabled) to avoid CRIME-style
issues and unbounded inflate sizes.

### 4.2 Messages

The normative wire format (byte layouts, limits, state machine, error codes,
and worked examples) is in [PROTOCOL.md](PROTOCOL.md). In short, every message
is one WebSocket binary message of at most 16 KiB: a type byte followed by a
fixed layout. Client to server: `Hello`, `Input`, `Resize`, `Ack`, `Ping`,
`Close`. Server to client: `Welcome`, `Output`, `Exit`, `Pong`, `Error`.
Authentication is not part of the protocol: the socket is only accepted after the
HTTP login (section 5). `Hello` must arrive within 5 seconds of the upgrade.

### 4.3 Versioning

`Hello.version` is the highest version the client speaks; `Welcome.version` is
the one chosen. Unknown versions are refused with `Error`. New optional
messages get new type numbers; there are no "ignore unknown" semantics.

### 4.4 Flow control and backpressure

The terminal can produce output faster than the browser parses and renders it
(`cat` of a big file), and the browser can produce input faster than the PTY
consumes it.

- Server -> client: the server tracks `sentOffset - ackedOffset`. When it
  exceeds a window (start with 256 KiB), it stops reading from the PTY master;
  the kernel then blocks the child, which is the correct behavior. The client
  sends `Ack` after parsing, at least every 64 KiB.
- Client -> server: `Input` is small and human-paced, but pastes can be large.
  The server queues at most 1 MiB per session (as `PtySession` already does)
  and closes the session with `Error` on overflow. The client chunks pastes to
  4 KiB frames.
- The server never buffers unbounded data for a slow client; a client that
  stops acknowledging for 30 seconds is disconnected (in v1 that ends the
  session, see 4.5).

### 4.5 Session lifetime and reconnection

**Version 1 behavior: a disconnect ends the session.** The server sends
`SIGHUP` to the process group when the socket closes (or after a short,
configurable grace period) and discards the session. `Hello` carries a session
id and resume offset anyway, and `Welcome` carries the `resumed`/`outputLost`
flags, so resume can be added later (phase 4) without changing the wire format;
a v1 server answers a non-empty session id with `Error`. The rest of this
section describes the target behavior for that later phase.

Sessions are independent of connections. The server keeps, per session, a ring
buffer of recent output (default 1 MiB) addressed by stream offset.

- New connection: `Hello` with empty session id creates a session and starts the
  configured program.
- Reconnect: `Hello` with a session id and `resumeOffset`. If the offset is
  still in the ring buffer, the server resumes sending from it. Otherwise it
  replies with `flags.outputLost` and starts at the oldest retained offset.
- When output was lost, the client resets its parser/model (`RIS`) and the
  server forces a redraw by re-sending the current window size (`SIGWINCH`
  after a one-cell size toggle). Full-screen programs repaint; a plain shell
  shows its prompt. Scrollback before the gap is lost. This limitation is
  inherent to keeping the emulator in the browser, and is accepted.
- A detached session is terminated after a grace period (default 5 minutes;
  `Detach` keeps it, `Close` ends it immediately). Only one attached client per
  session; a second attach replaces the first (the old one gets `Error`).
- At most one session per authenticated login to start with.

### 4.6 What the protocol deliberately does not have

No "run this command" message, no environment variables, no file or clipboard
access. The program and arguments are server configuration. The OSC 52
clipboard and OSC 8 hyperlinks stay disabled in the parser (VT.md).

## 5. Authentication and Access Control

### 5.1 Network exposure

- Default bind: `127.0.0.1` (and `::1`). Phase 1 supports nothing else.
- The server refuses to bind a non-loopback address unless TLS is configured,
  or it is explicitly told it sits behind a trusted reverse proxy that
  terminates TLS (`trusted_proxy`, below).
  A deliberate override, `allow_plain_http` (`--allow-plain-http`), permits plain
  HTTP on a non-loopback address for the container case, where the address is
  not routable from outside because the port is published on host loopback only.
  It is named loudly on purpose and logs a warning at start.
- Recommended public deployment: the host's existing nginx terminates TLS
  (HTTP/2, certificates, HSTS) and forwards to the server on loopback (or, with
  the Podman setup, to the container's loopback-published port). This keeps
  certificate handling out of this code base and avoids a second web server
  competing for ports 80/443. Native TLS in the server is optional later.
- `trusted_proxy` takes the proxy's exact address (implemented default: `none`,
  forwarding disabled). Only that peer may supply a client address through
  `X-Forwarded-For`, selecting the last hop appended by that proxy by default.
  Other peers retain their actual address; this is header trust, not an inbound
  connection ACL. Missing headers also use the peer; malformed or duplicate
  headers from trusted peers are rejected before authentication. When several proxies are chained, `trusted_proxy_hops` says how many entries
  to skip from the right (default 0, allowed 0..16). `trusted_proxy = any` is
  safe only when the listening port is reachable by the proxy alone, as with a
  loopback-published isolated container port. It prints a warning: network
  isolation is the operator's explicit assertion, not verifiable by the server.
  This also lets the server listen on a private container network
  address without being reachable by anything but the proxy.
- `localhost` is a secure context in browsers, so the `Secure` cookie flag and
  WASM work on the laptop without TLS.

### 5.2 Password and login

- One owner account. The passphrase is generated, not chosen: at least 24
  random characters (~140 bits) from a password manager, or a 6+ word
  diceware phrase. The server enforces a minimum length (24) when it is set.
- Storage: `matterm-server --set-password` reads the passphrase from stdin
  (never argv), hashes it with **Argon2id** (libsodium `crypto_pwhash_str`,
  interactive-or-higher parameters, ~100-250 ms), and writes the encoded hash
  to a credentials file under the config directory (section 5.6) with mode
  0600. libsodium is a small, widely audited C library used only for password
  hashing, random bytes, and constant-time comparison; Boost has no Argon2id,
  and these primitives should not be hand-written. Plaintext is never stored or logged.
- `POST /login` with `{"password": "..."}` over the same origin:
  1. Reject if the `Origin`/`Host` do not match the configured origin.
  2. Apply the rate limiter *before* hashing (5.3).
  3. Verify with `crypto_pwhash_str_verify` (constant time). Do a dummy hash
     when no password is configured so timing does not reveal state.
  4. On success, create a session token (`randombytes_buf`, 32 bytes), store
     only its hash server-side, and set the cookie
     `matterm_session=<token>; HttpOnly; Secure; SameSite=Strict; Path=/`.
  5. Failure responses are identical (same status, body, and rough timing).
- WebSocket upgrade requires a valid cookie *and* an `Origin` header equal to
  the configured origin. No credentials in URLs or query strings.
- Session tokens: idle timeout 12 hours, absolute timeout 7 days (configurable),
  rotated at login, invalidated by `POST /logout` and by password change.
- The page itself is served without authentication (a login form only); every
  other resource and the socket require the cookie.

### 5.3 Rate limiting and lockout

Even with a very strong passphrase this is defence in depth and a guard against
resource abuse. **Every number below is a configuration value** (section 5.6);
the values shown are defaults.

There are two independent mechanisms:

- **Per-source lockout** (affects only the offending address): after
  `max_failures` (5) consecutive failures, require an exponentially growing delay
  starting at `lockout_initial` (30 s), doubling each time, capped at
  `lockout_max` (15 min). Requests during the delay get `429` with `Retry-After`
  and never reach the hash function. A successful login clears that source's
  counter. Other addresses are not affected.
- **Global attempt budget** (affects everyone, including the owner): at most
  `global_attempts_per_hour` (30) password attempts in total across all sources;
  `0` disables it. It protects against distributed guessing from many addresses,
  but lets an attacker lock the owner out of the web login for the rest of the
  window. The owner's out-of-band way back in is running
  `matterm-server --reset-lockout` locally (or over SSH), and loopback logins
  (`exempt_loopback = true` by default) are not subject to the global budget.

Other rules:

- Failure state is persisted in the state directory so a restart does not reset
  the counters.
- Unauthenticated connections have a cap (`max_unauthenticated`, 16) and a
  5-second deadline to complete the request; slow-loris connections are dropped.
- The limiter takes an injectable clock so it is unit-testable.
- Because the settings are configuration, a lenient setup for the laptop and a
  strict one for the public server use the same code.

### 5.4 Process isolation and authorization

- Run `matterm-server` as an unprivileged user and refuse to start as root.
  The shell inherits that user's privileges, and a login to the web terminal is
  as powerful as a login to that account. Recommended for the public server: a
  **dedicated restricted user** (no sudo, no access to other users' files, ideally
  inside a container). Do administrative work through a separate, stronger
  channel (SSH with keys, then `sudo`), not through the web terminal. On the
  laptop during development, the normal account is acceptable because the
  server is only reachable from loopback.
- The server never offers a "run as root" or privilege-switching feature. If
  administering through the browser is ever wanted, that is a separate design
  that needs its own second factor and an audit log.
- Sample systemd unit with `NoNewPrivileges=`, `ProtectSystem=`,
  `PrivateTmp=`, and `RestrictAddressFamilies=`; or run in a container. These
  do not replace the account decision above.
- Environment for the child is rebuilt from a small allow-list plus `TERM`,
  `COLORTERM`, `LANG`; the server's own environment, in particular secrets, is
  never inherited (`PtyProcess` already filters TERM/COLORTERM/COLUMNS/LINES).
- Session cleanup: when a session ends, `SIGHUP` the process group, wait,
  `SIGKILL` after a timeout, close descriptors, and drop its buffers.

### 5.5 Second factor (not in v1, kept possible)

Authentication is a small staged pipeline in the server (`passphrase`, then
zero or more further stages), so adding a factor changes the login handler, not
the protocol. Candidates, in rough order of effort:

- **Approval over SSH.** After a correct passphrase the login stays pending
  until the owner runs `matterm-server --approve` (or answers a prompt) over an
  SSH session, for example from the phone. It needs no push-notification
  service and no extra app, and fits how the phone is already used. A pending
  login expires after a short timeout and shows the owner a short code to
  compare.
- **TOTP.** An authenticator app on the phone generates the code offline, so the
  server never has to send anything to the phone.
- **WebAuthn passkey.** Strongest and phishing-resistant, but needs a secure
  context/TLS and more implementation work.

Other possible hardening: a PAKE (OPAQUE) so the passphrase never reaches the
server even inside TLS (probably unnecessary when TLS is mandatory), client
certificates (mTLS) at the reverse proxy, and IP allow-listing at the proxy or
firewall.

### 5.6 Configuration and state

All server settings are configurable and live in files under the user's
configuration directory, so they can be saved and reused:

- Config: `$XDG_CONFIG_HOME/matterm/` (default `~/.config/matterm/`), for
  example `server.conf`. Credentials (the Argon2id hash) are in a separate
  `credentials` file with mode 0600, created by `--set-password`.
- State (lockout counters, session tokens' hashes): `$XDG_STATE_HOME/matterm/`
  (default `~/.local/state/matterm/`), mode 0700.
- Precedence: command-line flags override the file, which overrides built-in
  defaults. `--config PATH` selects another file; `--print-config` shows the
  effective values. Unknown keys are an error so typos in security settings do
  not silently fall back to defaults.
- Covered settings: listen address/port, `trusted_proxy`, allowed origin, TLS
  files, session and idle timeouts, the program and arguments to run, all rate
  limiting values in 5.3, protocol limits (4.2, 4.4), and scrollback/ring buffer
  sizes.
- *File format:* JSON, read with the project's own `lib/fast-json` library
  (header-only, a git submodule). Properties that matter for configuration:
  - JSON has no comments. Proposal: members whose name starts with `_` (for
    example `"_note"`) are ignored by the loader, and `--print-config` prints the
    effective configuration. The meaning of each setting is documented in a
    separate reference rather than inside the file.
  - "Unknown keys are an error" needs the loader to enumerate an object's
    members; `JsonNode` can iterate children, so this is feasible.
  - Writing the configuration back (settings saved from a GUI) uses `JsonOut`. The same directory is meant to hold the
  desktop terminal's future settings (see SETTINGS_AND_PERFORMANCE.md), which
  would live in a separate `terminal.conf`.

### 5.7 Build dependencies

Kept deliberately small: Boost (Beast/Asio are header-only; only
`boost_system` may be needed at link time), libsodium, and fast-json (header-only,
in `lib/fast-json`, no installation needed). On Debian/Ubuntu the system packages
are `libboost-dev libsodium-dev`. fast-json requires C++20 (it uses concepts) while
the rest of the repository builds as C++17; see the open question below.
The server build must not require SDL2, SDL2_ttf, fonts, or Fontconfig: the root
`CMakeLists.txt` currently runs `find_package(SDL2 REQUIRED)` unconditionally,
so phase 2 must make graphics dependencies conditional (`MATTERM_BUILD_APP`,
`MATTERM_BUILD_SERVER`). Missing dependencies produce a clear CMake error naming
the package to install.

## 6. Browser Client

- Build target `matterm-web` (Emscripten): reuses `matterm::terminal`,
  `matterm::vt`, the renderer, and `InputEncoder`. Native-only code (PTY,
  fontconfig, SDL window management) stays out of the build.
- Entry points are init / `tick(timestamp)` / shutdown driven by
  `requestAnimationFrame`, as the ROADMAP's Emscripten section already requires;
  redraw only on change or while scrolling/blinking.
- Data flow: WebSocket `Output` -> `VtParser::feed` -> model -> renderer;
  keyboard/mouse/paste/focus -> `InputEncoder` -> `Input` message; parser
  replies -> `Input` message; layout change -> `Resize`.
- Fonts: bundle the JetBrains Mono face as a WASM asset (FONTS.md already
  documents the pinned font); fallback fonts for Braille etc. are a later problem.
- Clipboard: copy uses a user-gesture `navigator.clipboard.writeText`; paste
  uses the `paste` event, not a clipboard read API.
- Content Security Policy: `default-src 'self'; script-src 'self'
  'wasm-unsafe-eval'; connect-src 'self'; frame-ancestors 'none'; object-src
  'none'; base-uri 'none'`. No third-party scripts, fonts, or analytics.
- **Rendering backend.** The first browser backend is the existing SDL2
  renderer built with Emscripten. Backend selection is a build option
  (`MATTERM_WEB_RENDERER=sdl|canvas|webgl`; only `sdl` initially). The
  renderer boundary is already the abstract `RenderBackend`
  (`render(const RenderFrame &)`, `present()`, in `src/render/renderbackend.h`),
  which `SdlBackend` implements. That costs one virtual call per frame, which is
  negligible next to drawing a frame, so a backend can also be chosen at run time
  later without a measurable performance loss. What the interface does not cover
  is cell metrics, column/row computation, and construction (the applications use
  `SdlBackend` directly for those); widening it is only needed once a second
  backend exists, and must not change native behavior or performance (verify
  with the benchmarks in BENCHMARKS.md).

## 7. Terminal Simulator (Later)

`ScriptedLink` implements `TerminalLink` by playing a timed list of
`(delay, bytes)` records, optionally echoing typed input through a tiny scripted
"shell". It uses the same parser, model, renderer, and `InputEncoder`. It needs
no server, no authentication, and no network, so it can be embedded on public
pages safely. The scripted format can start as JSON, and the existing
`--input FILE` / `--showcase` data can seed it. Recording real sessions with the
existing `--record-output` PTY recording gives realistic scripts.

## 8. Testing Plan

- `src/protocol`: round-trip tests for every message; truncated, oversized,
  and unknown-type inputs; a libFuzzer or randomized harness. Limits checked
  before allocation.
- Rate limiter and session store: deterministic tests with a fake clock
  (backoff schedule, persistence across restart, global budget, `Retry-After`).
- Auth: wrong password, missing/forged/expired cookie, wrong `Origin`, upgrade
  without cookie, constant-shape failure responses, minimum passphrase length.
- Server integration (loopback, ephemeral port): a test client connects, logs
  in, runs `echo` through bash, resizes, observes `Exit`; disconnect/reconnect
  with and without output loss; flow-control stall with `yes`; second-attach
  replacement; session cleanup (no leaked processes/descriptors).
- Browser: a headless-browser smoke test (Playwright or similar) that loads the
  page, logs in, and sees a prompt; manual checks for IME, paste, and mouse.
- Security review checklist before binding anything but loopback: external port
  scan, TLS configuration test, header review (CSP/HSTS/cookies), and a fuzz
  run on the protocol decoder.

## 9. Delivery Plan

Each phase is independently useful and ends with tests and documentation.

1. **Protocol and link interfaces** (done: `src/protocol/` codec and
   `ServerSession`; `src/transport/` `MessageChannel`, `TerminalLink`, and
   `ClientSession`, all with tests including a client-to-server end-to-end
   loopback), and `PtyLink` (`src/platform/posix/`), the native `TerminalLink`
   over a local PTY; `PtySession` is now built on it, so the native application
   uses the same interface as the web client.
2. **Local server, no browser.** `matterm-server` on loopback: configuration
   files, static files, `/login` with Argon2id, configurable rate limiter,
   WebSocket, sessions, flow control; a native test client in the test suite.
3. **Browser client.** Emscripten build, WebSocket transport, JS glue, CSP;
   `localhost` use against the phase 2 server.
4. **Reconnection and hardening.** Resume with the output ring buffer (v1 ends
   the session on disconnect),
   lockout persistence, systemd unit, header audit, fuzzing.
5. **Public deployment.** Reverse proxy and TLS, optional second factor,
   operational docs (backups of state file, password rotation, log review).
6. **Terminal simulator.** `ScriptedLink`, script format, embedding docs.
7. **WebRTC data channel** only if there is a concrete need (NAT traversal
   without opening a port, lower latency): needs a signaling path and a native
   library (for example libdatachannel). Terminal data needs reliable ordered
   delivery anyway, so it gains little over WebSocket on a normal server.

## 10. Container Deployment

The first deployments run in Podman, with the same compose files on the laptop
and on the public server (`deploy/podman/`, provisional until the server
exists). One container: the server with its shell on an `internal` network with
no outbound access; its single port is published on host loopback only, and
the host's own nginx terminates TLS on the server. Root filesystems are
read-only, all capabilities are dropped, and resources are limited. Inside the container the
shell can only see what is mounted, which also answers most of the account
question in 5.4: the container, not the host account, is the isolation boundary.
The host's config and state directories are mounted in, so native and container
runs share one layout for now.

## 11. Decisions

Made:

1. **Server language and libraries:** C++17 in this repository, reusing
   `PtyProcess`. Boost.Beast/Asio for HTTP and WebSocket; libsodium only for
   Argon2id, random bytes, and constant-time comparison.
2. **TLS:** a reverse proxy terminates TLS for the public server; the host's
   existing nginx is used rather than a second proxy on ports 80/443. Native TLS
   is optional later. Loopback needs none.
3. **Account:** a dedicated restricted user on the public server; administration
   through a separate channel (SSH, then sudo), not the web terminal. The normal
   account is acceptable on the laptop while loopback-only.
4. **Rendering:** SDL2 under Emscripten first; backend chosen at build time, with
   a virtual `RenderBackend` interface so a run-time choice stays possible.
5. **Reconnect:** v1 ends the session on disconnect; resume is a later phase and
   the wire format already reserves it.
6. **Lockout:** per-source lockout and a global attempt budget both exist, and
   both are fully configurable (including disabling the global budget).
7. **Second factor:** none in v1; the auth pipeline leaves room for approval
   over SSH, TOTP, or WebAuthn.
8. **Configuration:** everything configurable, stored under
   `~/.config/matterm/` (state under `~/.local/state/matterm/`).

9. **Configuration format:** JSON via the project's own `lib/fast-json` (replaces the
    earlier TOML suggestion). Exact key names are chosen during phase 2, when the
    settings exist.
10. **Containers:** Podman first, with a no-egress server network and a single
    loopback-published port (section 10); no proxy container in v1. The container starts out using the host's config and
    state directories.

Open:

- C++ standard: fast-json needs C++20, the repository uses C++17. Either the
  server target builds as C++20 (simplest; the protocol library can stay C++17 so
  the WASM client is unaffected), or fast-json gains a C++17 mode.
- fast-json housekeeping: its CMake file defines a standalone project
  (executable, tests), so the server should use only the `FastJson::FastJson`
  interface target; its README is a single line and needs usage documentation
  (parsing, typed access, `JsonOut`, error handling), which we plan to write.
  Also decide whether to track a pinned commit in this repository.
- Whether the container should keep sharing the host's config directory.
  Sharing is convenient, but the shell can then read the credentials file (the
  Argon2id hash) and the state directory. A separate, container-owned config and
  state volume, or mounting only the settings read-only and keeping credentials
  elsewhere, would be safer. Revisit before the public deployment.
- Whether the desktop terminal's settings share the config directory/loader.
- Which second factor to build first when it is wanted.
- Canvas/WebGL backends: only if SDL2 under Emscripten proves too large or slow.
