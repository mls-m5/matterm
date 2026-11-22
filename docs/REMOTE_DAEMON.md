# A daemon on another computer (design note and prototype, 2026-11-22, agent2)

Goal: a native matterm window on this computer shows the terminals of a `matterm-daemon` that runs
on another one, ideally with the same mechanism as the web version (docs/PROTOCOL.md section 12,
docs/SERVER_CONFIG.md). Three ways were looked at; the first and third are the same mechanism and are
built; the second is not.

## Recommendation

Use **ssh's Unix-socket forwarding** (a and c below). It needs nothing new on the wire or on the
remote besides `matterm-daemon` itself, it gives authentication, encryption and key management
from ssh as they are, the daemon already accepts exactly the account it belongs to, and the native
app already talks to a daemon through a socket path. `matterm --daemon-host user@host` does all of it.
Do the WebSocket client (b) only if "no ssh, but a web server with a password" is a wanted case; it
is more work, offers less (only shared sessions) and needs TLS first.

## a) By hand: `ssh -L` and `--daemon-socket` (works today)

```
# remote: make sure the daemon runs and see where its socket is
ssh host matterm-daemon --ensure --print-socket        # prints e.g. /run/user/1000/matterm/daemon.sock
# local: forward that socket to a private path and use it
mkdir -m 700 ~/.cache/matterm-host && \
ssh -N -L ~/.cache/matterm-host/d.sock:/run/user/1000/matterm/daemon.sock host &
matterm --daemon --daemon-socket ~/.cache/matterm-host/d.sock
```

OpenSSH 6.7 or later on both ends (`-L local_socket:remote_socket`); `sshd` must allow stream-local
forwarding (`AllowStreamLocalForwarding`, default yes). The daemon's framing (4-byte length, then
the message of docs/PROTOCOL.md) is a plain byte stream, so ssh carries it unchanged. `--ensure` and
`--print-socket` are new options of `matterm-daemon`: `--ensure` (implies `--daemonize`) does nothing
when a daemon already answers on the socket, `--print-socket` prints the path (after the daemon is
up when combined with `--ensure`/`--daemonize`; the path depends on the remote `$XDG_RUNTIME_DIR`,
which the local side cannot guess).

What is not right when done by hand: `matterm --daemon-socket PATH` would start a local daemon on
PATH if nothing answers there yet (the tunnel not being up is "nothing answers"), the tunnel is
yours to start, watch and clean up, and the new-terminal folder is a folder of *this* computer.

## c) `matterm --daemon-host [user@]host [--ssh-arg ARG]...` (built)

`src/frontend/sshtunnel.h` (`frontend::SshTunnel`) does a) for the app:

1. `ssh -o BatchMode=yes -o ConnectTimeout=10 [--ssh-arg...] -- host matterm-daemon --ensure --print-socket`
   starts the daemon there if needed and reads the socket path from its last line of output.
2. A private directory (`mkdtemp`, mode 0700, under `$XDG_RUNTIME_DIR` or `/tmp`) and
   `ssh -N -o ExitOnForwardFailure=yes -o StreamLocalBindUnlink=yes -o ServerAliveInterval=15 -L LOCAL:REMOTE -- host`
   as a child process (its stderr goes to a file in that directory).
3. Wait until LOCAL accepts a connection, which means the remote daemon answered through the tunnel;
   fail with ssh's own words if the ssh process ends first, or after 20 s.
4. The app uses LOCAL as the daemon socket, as if the daemon were local, with these differences:
   `--daemon` is implied and required (no fallback to local programs), new terminals start in the
   remote home (a local folder means nothing there), the shared-marks poll runs every 10 s instead of 2 s
   (each question waits for the answer, and the link may be slow), and the connection comes back by
   itself when it ends (see "Reconnect"). Leaving the window kills the ssh child and removes the socket; the terminals keep running in
   the remote daemon, as with a local one, and a new `matterm --daemon-host` finds them again.

`--ssh-arg` arguments come first, so they win over ours (ssh keeps the first value of an option):
`--ssh-arg -p --ssh-arg 2222`, `--ssh-arg -oBatchMode=no` for a password prompt (needs a terminal), a
`ControlMaster` in `~/.ssh/config` makes the second ssh call (the forward) reuse the first one's
connection. Host, daemon name and socket path are checked against a short list of safe characters
before anything runs (the remote command line goes through the remote shell, and a host starting
with `-` would be an ssh option).

### Copying the daemon there (`--daemon-install`, 2026-11-22)

`matterm-daemon` has to exist on the other computer. By default `--daemon-host` uses the one on its
PATH (a remote non-interactive shell often has no `~/.local/bin`, and `.bashrc` is not read); when there
is none it offers to copy ours. `--daemon-install` always uses our own copy; `--yes` copies without asking.

**What the binary needs.** The normal build links `libsodium.so.23`, `libstdc++`, `libgcc_s` and glibc
dynamically (`ldd build/matterm-daemon`), so it only runs where those exist, in these versions. The
option `-DMATTERM_STATIC_DAEMON=ON` links it fully static (`-static`, libsodium from `libsodium.a`; needs
`libc.a`, in Debian `libc6-dev`, and `libsodium-dev`): one 1.7 MB file, "not a dynamic executable", that
runs on any Linux of the same CPU architecture without packages. The daemon does no name lookups
(no `getpwnam`, no DNS), which is what makes a static glibc safe here; musl was not needed. `sshtunnel_test`
passes with the static build (`MATTERM_TEST_DAEMON=path ./test/sshtunnel_test`), including a terminal
running in it. A copy of a dynamic binary is still made, with a note, and the check below shows if it runs.

**Steps** (`SshTunnel::installRemote`, `src/frontend/sshtunnel.h`):

1. Our file: `matterm-daemon` beside this program (else PATH); its SHA-256 and size (`sha256.h`).
2. One ssh round trip: `uname -sm`, the folder (`$HOME/.local/share/matterm/bin`, created 0700) and whether
   `matterm-daemon-<first 24 hex of the hash>` is there and executable there.
3. Another OS or CPU architecture than ours is a plain error ("is Linux aarch64 and this matterm-daemon is
   for Linux x86_64 ... build matterm-daemon on that computer and put it on its PATH"), nothing is copied.
   Building there is the fallback: `cmake -DMATTERM_BUILD_SERVER=ON -DMATTERM_STATIC_DAEMON=ON` and
   `cp build/matterm-daemon ~/.local/bin/` (it needs Boost headers and libsodium-dev to build).
4. Present: nothing more. Missing: ask ("host has no matterm-daemon of this version. Copy ours (1745 KiB,
   DIR/NAME) and start it there? [y/N]", on the terminal; with no terminal the answer is no unless `--yes`).
5. Copy: the file is streamed as ssh's stdin into `DIR/.matterm-daemon.$$` (umask 077, mode 0700), checked
   for size, SHA-256 (when `sha256sum` exists) and that it runs at all (`--print-socket`), and only then
   `mv`-ed to the final name, atomically. Failure removes the temporary file. Errors carry hints: wrong
   format (architecture), `noexec` or unwritable home, missing libraries of a dynamic binary.
6. It is started by its full path (`DIR/NAME --ensure --print-socket`), so nothing depends on PATH or
   `.bashrc`.

**Versions.** The name is the content, so a new version is a new file and one that is running is never
overwritten or killed. A daemon that already runs on the socket is reused whatever its version, and with
sessions in it never restarted; the old file stays (about 1.7 MB each; delete old `matterm-daemon-*` by
hand when no daemon runs from them). If the running daemon speaks an incompatible protocol the connection
fails with the daemon's own `ControlHello` error: stop it (palette "Stop daemon", or kill it) when it has no
sessions. There is no automatic restart.

**Safety.** Only our own binary is ever sent; it is read from beside this program or PATH, never a path
taken from the remote. No sudo, nothing outside `~/.local/share/matterm/bin` is written (the folder path is
validated against a short character list, the remote shell only sees validated words and one fixed script),
the first copy needs a yes, and the file is content-named, 0700, and renamed into place.

### Security

* Remote side: sshd runs the forwarded connection as the account you logged in as, so the daemon's
  `SO_PEERCRED` check (uid equals the daemon's own) accepts exactly that account and nobody else; the
  daemon does not learn anything about ssh. Whoever can log in as that account can already start a
  shell there: the tunnel adds no privilege. Root on the remote can of course reach any socket.
* Local side: `UnixSocketChannel` checks that the peer of the socket is our own uid, which it is: the
  ssh child. Other local users cannot even reach the socket, its directory is 0700. Another user
  who plants a socket under `/tmp` is refused by that check as before.
* No agent forwarding, no passwords, no tokens are involved; ssh does host key checking (with
  `BatchMode=yes` an unknown host fails instead of asking: run `ssh host` once first).
* Nothing listens on a TCP port on either side.

### Reconnect

`frontend::TunnelReconnector` (`sshtunnel.h`) keeps the tunnel up. The main loop calls `tick()` every
round; it never blocks.

* **Detecting**: the ssh child ended (network drop, laptop sleep: `ServerAliveInterval=15` with 3 misses
  ends a silent link in about 45 s). The panes notice by themselves (their socket closes, state
  "disconnected") and keep their screens.
* **Retrying**: on a worker thread `SshTunnel::reconnect()` stops the old ssh, asks the remote daemon
  again where its socket is (`--ensure`: it is started there if it is gone), and makes the same
  `ssh -L` forward at the same local socket, with the same options. Attempts come 0, 1, 2, 5, 10 and
  then every 30 s after the loss or the previous failure (`ReconnectPolicy`), at most 8. Nothing is
  copied again; a daemon copy that has disappeared is an error.
* **Reattaching**: when the forward answers, every pane whose state is "disconnected" attaches again
  with its session id and the offset it had received (`reattachToDaemon`), so the daemon replays
  exactly what was printed meanwhile, no more and no less. A pane that was taken over by another window
  stays as it is.
* **Giving up**: after the last failed attempt the window says why, the panes say "disconnected from
  HOST, press Enter to retry", and nothing more is tried by itself.
* **By hand**: the palette entry "Reconnect" (only with `--daemon-host`) or Enter in a disconnected
  pane retries at once with the counting from zero. This also replaces an ssh that is alive but stuck;
  Enter does that when the pane still cannot attach although the tunnel looks up.
* Closing the window during an attempt cancels the running ssh call; it does not wait for the 20 s.
* Tests (`test/sshtunnel_test.cpp`, against `test/fakessh.py`): ssh is killed while a program prints and
  80 numbered lines arrive once each in order; back-off and giving up; retry by hand; a slow link
  (`FAKE_SSH_DELAY`) never makes `tick()` take more than 200 ms; close during an attempt.

### Not covered by the prototype

* Opening the tunnel the first time blocks start-up (up to 20 s, one line on stderr); a reconnect does
  not block. No dialog, no palette entry to connect to a second host, one host per window.
* The reconnect has never run against a real sshd or a real network drop, and the window code (notes,
  Enter, the palette entry) was only started headless: killing the forward ssh made a new one appear,
  but nobody saw the notes on a real display.
* Blocking questions to the daemon (status, share) from the UI thread wait for the round trip; on
  a slow link that stutters. They should move to the link's own connection.
* Version mismatch is only what `ControlHello` already reports; `matterm-daemon` must be on the
  remote `PATH` or be copied with `--daemon-install`.
* Session restore and "new terminal in this folder" use local folders and are off for a remote daemon.
* The copy is for Linux on the same CPU architecture as this computer (see above for the fallback).
* Windows has no `ssh -L` for Unix sockets worth relying on, and no daemon.
* Real `ssh` was not available where this was built: the tests run the whole chain against a fake
  `ssh` that relays Unix sockets (`test/fakessh.py`); the ssh options used are the documented ones
  but have not been run against a real sshd here.

## b) A native WebSocket client against `matterm-server --expose-local-sessions` (not built)

The web version reaches a daemon through `matterm-server`: it logs in (password, `POST /login`,
cookie, strict `Origin`/`Host`), lists the shared sessions (`POST /local-sessions`) and opens
`/ws` with `local:<id>`; the server attaches to the daemon through an exposed connection
(docs/PROTOCOL.md "Exposed connections"). The messages on that WebSocket are the same
`ClientMessage`/`ServerMessage` as in the daemon's frames, and the app's `MessageChannel` interface
(`src/transport/messagechannel.h`) is exactly the place for a WebSocket implementation, so
`ClientSession`/`DaemonLink` would be reused unchanged. Missing:

* A WebSocket client channel in the app (Boost.Beast is only linked into the server; SDL builds have
  raw sockets only), an HTTP client for the login and the listing, cookie handling.
* **TLS.** The server does plain HTTP/WebSocket (SERVER_CONFIG: TLS only through a proxy on the same
  machine), so over a network it needs a reverse proxy with TLS in front, or `ssh -L` of the TCP port
  anyway, which makes a) simpler.
* Credentials: the password has to be entered or stored in the native app (keyring); the server's
  per-source attempt limits and `Origin` check were made for browsers (a native client must send the
  configured origin).
* Only sessions the owner has *shared* are visible, and no new terminals can be made (the exposed
  connection is limited to listing and attaching): a remote desktop-like use needs a policy decision
  on that, a remote `matterm-server` running all the time, and `--allow-remote-listen` (a shell
  for anyone with the password: SERVER_CONFIG warns).

(b) therefore suits "look at a terminal somebody shared, from anywhere with a password" and not
"my machine's terminals on my other machine", which is what a) and c) do.
