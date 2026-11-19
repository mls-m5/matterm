# Security Review: server, browser client, deployment files

Scope: `src/server/**`, `src/protocol/**`, `web/login.js`, `web/shell.html`,
`web/main.cpp` (paste, title), `web/websocketchannel.*`, `deploy/podman/**`.
Threat model: a patient remote attacker who can send arbitrary bytes, plus a
hostile web page open in the owner's browser. Method: reading the code, then
probing the running server with raw sockets (`test/serversecurity_test.cpp`).
Base: `main` at debd8f0.

## Findings, ranked

### 1. High: the deployment files could not start the server (fixed)

`compose.yaml` and `compose.server.yaml` passed `--listen=0.0.0.0:8080`,
`--allowed-origin=...`, `--trusted-proxy=any`, `--trusted-proxy-hops=1` and a
`--health-check` probe. The flag parser accepts only `--flag value` pairs and
has none of the last four, so the container exits with "unknown option". An
operator who "fixes" this by deleting flags ends up in finding 2. Also, no
`--static-dir` was passed, so even a running container served only the
placeholder page, and the base file listed two origins where the server accepts
one. The README claimed the server trusted the proxy's `X-Forwarded-For`; it
does not.
Fix: flags rewritten to what the parser accepts (`--origin`, `--static-dir`,
separate `--listen`/`--port` items), the health check removed, README corrected.
Tests: "Deploy: compose commands are accepted by the real flag parser" and "the
compose files use no flag the server lacks" run the compose `command:` lists
through `parseCommandLine` and `validateSettings`.

### 2. High (availability), design decision pending: one attacker locks the owner out behind the proxy

The server has no `trusted_proxy`, so behind nginx every client has the proxy's
address (or the container gateway's). The per-source lockout is therefore one
shared lockout. After `maxFailures` (5) wrong passwords, the source is locked
for 30 s, doubling to 15 min. A failure after each expiry re-arms the maximum,
so one wrong request per 15 minutes keeps the owner out indefinitely. If the
peer is not loopback, 30 failures per hour also exhaust the global budget for
every login. If the peer is loopback (native nginx), the global budget is
exempt, so the per-source lock is the only control. Guessing is not made
easier: a 24+ byte Argon2id password stays out of reach; only availability
suffers. docs/SERVER.md already records this as residual risk.
Mitigation shipped: the nginx example now limits `/login` to 6 requests per
minute per client address (burst 3), which does not stop a patient attacker
from holding the shared lockout open but makes the budget per client rather
than global once the server can key on it.
Options (not chosen here): (a) implement `trusted_proxy` as specified in
docs/WEB_TERMINAL.md section 5 and key limiter and lockout on the last
`X-Forwarded-For` hop; (b) fail2ban or `limit_req` at the proxy only, and run
the server with the limiter set to lockouts that do not escalate for the
loopback peer; (c) a documented recovery (a `--reset-lockout` command or a
SIGHUP that clears the limiter). (a) is the intended design and needs no
protocol change.
Test: "disabled Lockout of one source does not lock out another" is written
and disabled until (a) exists.

### 3. Low: no cross-origin isolation headers (fixed)

Responses had CSP `frame-ancestors 'none'`, `nosniff`, `no-store` and
`Referrer-Policy`, but no `Cross-Origin-Opener-Policy` or
`Cross-Origin-Resource-Policy`. A hostile page that opens the terminal in a
window (or is opened from it) kept a `window.opener`/handle relationship, and
the responses could be embedded as cross-origin resources. Fix: both headers
(`same-origin`) on every response. Test: "Every response carries the isolation
and caching headers".

### 4. Low: nginx example gaps (fixed)

No `ssl_protocols` (nginx before 1.23.4 enables TLS 1.0 and 1.1 by default),
no rate limit on `/login` (the only per-client limit, see finding 2), default
timeouts of 60 s for slow request headers and bodies, and unknown `Host` names
reached the server block. Fixed: `ssl_protocols TLSv1.2 TLSv1.3`,
`limit_req_zone` plus `location = /login`, 10 s header and body timeouts, and a
`default_server` with `ssl_reject_handshake on`. Test: "Deploy: nginx example
limits TLS versions, logins and slow clients" (text assertions only; nginx was
not run).

### 5. Low, accepted: memory amplification for static files

Each static response copies the whole file into the response (`string_body`).
Up to 16 unauthenticated connections times the largest file (the WebAssembly
module) can be in flight, with no hashing or rate limit involved. Behind nginx
(which reads requests completely before forwarding) an attacker cannot hold
16 slots cheaply, and the container limit is 512 MiB. Cheap future fix: serve
files from a shared buffer instead of a copy. Not changed.

### 6. Low, accepted: session cookie has no `__Host-` prefix

`matterm_session` is `HttpOnly; Secure; SameSite=Strict; Path=/` without
`Domain`. A sibling subdomain could still plant a second cookie with the same
name; the server rejects duplicate cookies, so the effect is a denial of the
WebSocket upgrade until the cookie is cleared, not a fixation (tokens are only
issued by the server, 256 bits, compared as digests). `__Host-` would break the
plain-HTTP laptop setup on some browsers; decide with the TLS work.

### 7. Informational

- A rejected upgrade because of early buffered data happens after the session
  claim, burning the token (re-login needed; costs the attacker nothing it
  could not already do and needs the cookie).
- The shell runs as the server's user, so a logged-in user can read the server's
  memory, its config directory (the password hash) and kill it. This is the
  product (a shell for the owner); isolation comes from the container.
- No `RLIMIT_*`, `PR_SET_DUMPABLE` or core-dump setting; the container's
  `pids_limit` and memory limit are the bounds.
- The accept loop re-arms immediately after an error such as `EMFILE`; with
  `maxConnections` 32 this cannot spin in practice.
- `connect-src 'self'` must cover `ws:`/`wss:` for the same host. Current
  browsers do; older Safari did not (not verified here).

## Checked and found fine

- Login: rate limit and lockout run before hashing; unknown, wrong, malformed
  and unconfigured paths all hash and return the same body; password never
  logged; request body zeroed; bodies over 512 bytes and headers over 8 KiB are
  rejected; Argon2id parameters are checked on load; the credentials and config
  files are opened with `O_NOFOLLOW`, then `fstat`-checked (owner, mode) on the
  descriptor, so there is no check/use race; `--set-password` writes a 0600
  temporary file with `O_EXCL` and renames.
- Session: 256-bit random token, only a BLAKE2b digest kept, constant-time
  compare, single upgrade claim, rotation on every login (the old WebSocket is
  closed with 1008), idle and absolute timeouts, generation checks on every
  message and tick.
- Cross-site: `Origin` and `Host` must each appear once and match the single
  configured origin exactly; `null`, trailing dot, case, port, scheme, path,
  userinfo and duplicate Host variants are all refused (tested). CSRF on
  `/login` and CSWSH on `/ws` need a matching `Origin`, which a browser will not
  forge. `SameSite=Strict` as a second layer. CSP denies everything but
  same-origin scripts (plus `wasm-unsafe-eval`), styles, images, fonts and
  connections, no inline script, `frame-ancestors 'none'`, `base-uri 'none'`,
  `form-action 'none'`. All responses are `no-store` and `nosniff`.
- HTTP and WebSocket: one request per connection (`keep_alive(false)`), so a
  pipelined or smuggled second request is never answered (tested, including
  Content-Length plus chunked); early WebSocket data is rejected; deadlines on
  request read, response write, handshake, close and idle; message size 16 KiB,
  message rate, resize rate, input queue 1 MiB, output queue 512 KiB and 1024
  messages, ack window and stall timeout; compression disabled; text frames
  close with 1002. Static files: loaded once, regular non-symlink files directly
  in one directory, exact-name lookup, no decoding (`%2e%2e`, `..`, `//`,
  dotfiles all 404, tested), 64 MiB total cap, `nosniff`.
- Browser client: `login.js` writes messages with `textContent`, never
  `innerHTML`; the page opens `ws(s)://` to its own host only; the password
  field is cleared after submit; paste goes through `InputEncoder::paste`, which
  drops every C0 control including ESC, so a clipboard text cannot close the
  bracketed-paste markers or inject sequences; the program-to-browser path
  handles OSC 0/2 (title via `document.title`, never markup), has no OSC 52
  clipboard access, and terminal replies (DA, DSR, OSC 4/10/11 queries) never
  echo program-chosen text; `window.open` uses `noopener,noreferrer`.
- Process and host: root refusal in both entry points; descriptors above stdio
  closed in the child (`close_range` with a fallback loop); environment is an
  allow-list; process group HUP then KILL, reaped, also on transport loss,
  shutdown and a HUP-ignoring shell; PTY reads are bounded per pump, so a flood
  cannot starve the loop or grow memory.
- Deployment: ports published on loopback, read-only root, all capabilities
  dropped, `no-new-privileges`, non-root user, pid/memory/CPU limits, tmpfs home
  with `noexec`, internal network, config mounted read-only, forwarded address
  overwritten (not appended) by nginx, `Host` forwarded, WebSocket upgrade map,
  HSTS, `nosniff`, `server_tokens off`, 16 KiB body limit.

## Not checked

- Containers cannot run here: none of the compose, Containerfile or nginx
  behavior was executed (only parsed with the server's own parser or read).
  Unverified in particular: a published port on an `internal` network, the
  peer address the server sees from rootless Podman (loopback, gateway or the
  real client), setuid binaries in the Debian image under `cap_drop: ALL`, and
  that the image `COPY web/` carries source files, not the Emscripten build
  output.
- The browser client itself (no Emscripten): CSP compatibility of the generated
  `index.js`/`index.data`, `connect-src` with WebSocket in each browser, and the
  behavior of `web/main.cpp` link opening (owned by another task).
- TLS and nginx configuration against a live handshake; OS-level settings
  (firewall, resource limits, swap); denial of service by volume (bandwidth,
  connection floods at the network layer); timing side channels below the
  granularity of a loopback test; the terminal emulator's parser beyond the
  fuzz tests already integrated; dependency vulnerabilities (Boost, libsodium).
