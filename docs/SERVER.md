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

## Run

```
# Set the password (24..256 bytes). Read from stdin, never from argv.
printf '%s\n' 'a very long passphrase ...' |
    build/server/src/server/matterm-server --set-password \
        --credentials ~/.config/matterm/credentials

build/server/src/server/matterm-server \
    --credentials ~/.config/matterm/credentials \
    --origin http://localhost:8080 [--static-dir build-web/site] [-- /bin/bash -l]
```

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

TLS, config files (everything is a command-line option), second factor, session resume, persistent lockout state. Open review
items are in [tasks/server-skeleton.md](tasks/server-skeleton.md).
