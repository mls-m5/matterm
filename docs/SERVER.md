# matterm-server

The first slice of the server described in [WEB_TERMINAL.md](WEB_TERMINAL.md)
and speaking the protocol in [PROTOCOL.md](PROTOCOL.md). It authenticates one
owner, upgrades to a WebSocket, and runs one fixed program on a PTY per
connection. Linux only.

## Build

Needs Boost (headers, 1.74+) and libsodium. SDL is not needed.

```
cmake -S . -B build/server -DMATTERM_BUILD_SERVER=ON \
      -DMATTERM_BUILD_APP=OFF -DMATTERM_BUILD_DEMO=OFF
cmake --build build/server
ctest --test-dir build/server
```

## Credentials (the password file)

The server never stores your password. The "credentials file" holds one line, an
Argon2id hash of it, and the server compares what you type at login against that
hash. Create it once, with the helper script, which asks for the password twice
(nothing is shown, and it never appears on a command line or in the shell
history) and writes the file with mode 0600:

```
tools/set-password.sh build/server/src/server/matterm-server
# writes ~/.config/matterm/credentials (or give another path as 2nd argument)
```

[docs/examples/credentials.example](examples/credentials.example) shows what the
file looks like. It is not a working file: the hash is made up, and the server
refuses it on purpose. Never copy a hash from an example, make your own, and
keep the file private (the server refuses a file that others can read). To
change the password, run the script again. Use a long passphrase: the length is
what makes guessing infeasible.

## Run

```
# Set the password (24..256 bytes). Read from stdin, never from argv.
printf '%s\n' 'a very long passphrase ...' |
    build/server/src/server/matterm-server --set-password \
        --credentials ~/.config/matterm/credentials

build/server/src/server/matterm-server \
    --credentials ~/.config/matterm/credentials \
    --origin http://localhost:8080 [-- /bin/bash -l]
```

Settings can also live in a JSON file (`~/.config/matterm/server.json` or
`--config PATH`); flags override it. `--print-config` shows the effective
values. Every key is described in [SERVER_CONFIG.md](SERVER_CONFIG.md).

- Listens on 127.0.0.1 only. Any other `--listen` address needs
  `--allow-plain-http` and prints a warning (put TLS in front of it).
- Refuses to run as root. The program run on the PTY comes only from the
  command line after `--` (default `/bin/sh`), never from a client.
- The credentials file must be a regular file owned by the user with mode 0600.
  Without one, every login fails (after the same Argon2id work).
- `--origin` is the exact `Origin` browsers must send, and its authority must
  equal the `Host` header.

## HTTP interface

| Request | Result |
| --- | --- |
| `GET /` and other client files | With `--static-dir DIR`: the regular files of DIR, by exact name (`/` is `index.html`; read once at startup, 64 MiB max, no subdirectories or dotfiles), with a script-only CSP. Without it: a placeholder page. The browser client is built by `web/` (see web/README.md). |
| `POST /login` `{"password": "..."}` | 200 and `matterm_session` cookie (HttpOnly, Secure, SameSite=Strict); 401 otherwise; 429 with `Retry-After` when rate limited. |
| `GET /ws` (WebSocket, subprotocol `matterm-tty.v1`) | Needs the cookie and the exact Origin. One upgrade per login. |

Every login rotates the single owner session. Limits: 8 KiB headers, 512 byte
bodies, 5 s to authenticate, 16 unauthenticated and 32 total connections,
16 KiB WebSocket messages, no compression, 512 KiB / 1024 queued messages per
connection.

## Rate limiting

Two mechanisms from [WEB_TERMINAL.md](WEB_TERMINAL.md) section 5.3, both
checked before any password hashing. Values live in `RateLimitConfig`.

- **Per-source lockout.** After `maxFailures` (5) consecutive failures a source
  is refused for `lockoutInitial` (30 s), doubling to `lockoutMax` (15 min)
  while it keeps failing. A success clears it. The source key is the peer
  address: IPv4 as is, IPv4-mapped IPv6 as the IPv4 address, any other IPv6
  address as its /64 prefix (`sourceKey()`, so a client cannot rotate through
  the addresses of its own /64).
- **Source table.** At most `maxSources` (4096) sources are remembered. A source
  with no active lockout that has been idle for `sourceIdleExpiry` (1 h) is
  forgotten, by a scan that runs at most once per second. When the table is
  still full, a new source evicts the unlocked source with the fewest failures
  (oldest on ties), so a scan from many addresses cannot lock the owner out.
  An active lockout is never evicted; only if every slot holds one is a new
  source refused, with a `Retry-After` of the earliest lockout's end. The cost
  of eviction is that an attacker who pushes thousands of other sources through
  the table can reset the partial failure count of one unlocked source, which
  takes thousands of hashed attempts and the global budget below.
- **Global budget** (`globalAttemptsPerHour`, 30, 0 disables; loopback exempt
  by default). Decision: only failures spend it (`globalCountsFailuresOnly`,
  default true). An attempt reserves a slot before hashing, so the number of
  concurrent hashes stays bounded, and a successful login gives the slot back.
  Reason: the budget exists to stop distributed guessing, and guesses are
  failures; the owner logging in from several devices should not eat into it.
  Set `globalCountsFailuresOnly = false` to count every attempt.
  Residual risk, unchanged by this choice: whoever fails 30 times in an hour
  (from any addresses) blocks every non-loopback login until the hour ends.
  The way back in is a login from loopback (exempt), or restarting the server
  (the state is in memory only). A `--reset-lockout` command needs persisted
  state and is deferred with it. Until `trusted_proxy` exists, clients behind a
  reverse proxy all appear as the proxy's loopback address: they share one
  per-source lockout and are exempt from the global budget.

`SessionStore` and `RateLimiter` are not thread safe. They belong to the single
`io_context` thread; debug builds assert it (the first calling thread becomes
the owner).

## Deadlines and shutdown

Every wait on a peer has a bound (`ServerConfig`, defaults shown):

| Situation | Bound |
| --- | --- |
| Request and response before authentication | `requestTimeout` 5 s in total; a trickle of bytes does not extend it |
| WebSocket handshake, and the closing handshake the peer never answers | `webSocketHandshakeTimeout` 5 s |
| No message received on an open WebSocket | `webSocketIdleTimeout` 60 s. Beast arms it whenever a read starts; WebSocket pings do not extend it |
| No Hello / no protocol message / no Ack of pending output | `session.helloTimeout` 5 s, `idleTimeout` 60 s, `ackStallTimeout` 30 s, then Error and close |
| Peer that does not drain sent data | Queue limit of 512 KiB / 1024 messages: the connection is dropped at once |

The PTY is cleaned up whichever of these ends a connection: the process group
gets SIGHUP, then SIGKILL after the grace period. Stopping the server
(SIGINT/SIGTERM, or `io_context::stop()` followed by destroying the `Server`
and the context) drops every connection without a protocol Close and kills the
process groups at once, without the HUP grace; the shells and their
background processes are gone and reaped when the destructors return. This is
covered by `test/serverlifecycle_test.cpp`. A write stalled on a full kernel
buffer cannot be provoked on loopback with the default 256 KiB window, so that
case is covered by the queue limit test and by the Beast timer behavior above,
not by a test of its own.

## Process lifecycle

The child gets a small environment allow-list plus `TERM=xterm-256color` and
`COLORTERM=truecolor`, and no inherited descriptors besides stdio.

- The end of the leader (exit, disconnect, protocol Close or error) sends
  SIGHUP to the whole process group, and SIGKILL after 500 ms. The leader stays
  unreaped until then, so background processes cannot outlive the session.
- Exit is sent only after all output was read. If the client asked to close and
  never acknowledged output, waiting for PTY EOF is abandoned after 1 s and the
  transport closes (1000) without an Exit message.

## Not in this slice

TLS, `trusted_proxy`, writing the configuration back, second factor, session resume, persistent lockout state. Open review
items are in [tasks/server-skeleton.md](tasks/server-skeleton.md).
