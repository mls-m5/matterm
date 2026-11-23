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

### Versions, several daemons, `matmux` (2026-11-23)

**Names.** `--daemon-install` copies two files into `~/.local/share/matterm/bin/` on the other computer,
`matterm-daemon-<format>.<breaking>.<additive>-<hash8>` and the same for `matmux` (a static
program without SDL: `--attach-tty`, `--daemon-list`, `--daemon-stop`), and points the stable names
`matterm-daemon` and `matmux` in that folder at them. So `ssh -t you@computer
'~/.local/share/matterm/bin/matmux'` always works (no `.bashrc` change; put the folder on PATH
only if you want the short name). A running file is never replaced; after copying, old versions are
removed: only files with exactly those names, never one that a process runs (`/proc/*/exe`), the newest
older version stays as a fallback, no question, and what was removed is printed. Versions are compared
as numbers (1.10.0 is newer than 1.9.0). Files from before (named after their content) are left alone.

**Versions.** The daemon's control protocol is `format.breaking.additive` plus the build hash
(docs/PROTOCOL.md "Control version"). A different *breaking* number is refused with an explanation
(which version runs, how many terminals it keeps) and, on this computer, an offer to start a new daemon
on its own socket `daemon-<version>.sock` next to the old one. Over `--daemon-host` nothing is offered:
the message says how to list and stop. A different *additive* number is only a note.

**Seeing and stopping.** `matterm --daemon-list` (or `matmux --daemon-list`) lists the daemons of
this user: socket, pid, version and hash, number of terminals, and whether it can work with this build.
`--daemon-stop` asks first ("This ends N terminals"), then asks the daemon to shut down; a daemon that
does not answer the handshake (too old) is found by the pid of the process behind the socket
(`SO_PEERCRED`, same user only) and sent SIGTERM. With several daemons, name one with `--daemon-socket`.
With `--daemon-host` the same commands run `matmux` on the other computer (copied there first if
needed, as for the daemon). `matterm-daemon --ensure` reuses a compatible daemon on any socket name.

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
* Session restore, Attach tab... and "new terminal in this folder" pass the saved or current folder (as
  the daemon saw it, by `GetSessionInfo`) on to the daemon, which looks at it on its own computer and
  starts in its home folder when it is missing; the pane then says "restored without its folder".
* The copy is for Linux on the same CPU architecture as this computer (see above for the fallback).
* Windows has no `ssh -L` for Unix sockets worth relying on, and no daemon.
* Real `ssh` was not available where this was built: the tests run the whole chain against a fake
  `ssh` that relays Unix sockets (`test/fakessh.py`); the ssh options used are the documented ones
  but have not been run against a real sshd here.

## d) From a phone, or without a window: `matterm --attach-tty` (built)

For a phone (Termux, JuiceSSH, any ssh client) there is no window and no browser needed. Log in to
the computer that runs the daemon and start the terminal mode there:

```
ssh -t you@your-computer matterm --attach-tty           # the list
ssh -t you@your-computer matterm --attach-tty --new     # a new terminal at once, in a fresh shell
ssh -t you@your-computer matterm --attach-tty --new --name work -- htop
```

(`matmux` is the same without the window libraries: `matmux` alone shows the list with **New
terminal** chosen (Enter starts a terminal with your shell; when no terminal runs at all it starts one at
once, no pointless list), `matmux --attach` is the same list with the first terminal chosen,
`matmux --new [--name N] [-- COMMAND]` starts one directly as above; `-t` is needed so that a terminal is
allocated.) Together with the daemon this is a small tmux: start a terminal, leave it running, come
back to it later.

### matmux passes the output through (2026-11-23)

`matmux` shows a terminal by **passing the program's output through** to your terminal, unchanged: your
terminal keeps its own scrollback, selection, mouse and touch scrolling (in Termux: swipe to scroll, long
press to select), and a program that is not in the alternate screen does not put your terminal there
either. When you attach, the daemon's replay is read into a hidden screen first and the host terminal is
brought to the same state in one go (the older lines first, so they land in the scrollback, then the screen
with the same rows); after that every byte goes through as it comes, in order, nothing dropped (when your
terminal is slower than the program, matmux stops reading from the daemon until it has caught up). Keys,
the mouse and paste go to the program as your terminal sends them. A resize goes to the daemon and the
program redraws itself.

Ctrl+B still is the prefix: `d` leaves (the terminal keeps running), `l` the list, `c` a new terminal,
Ctrl+B twice sends one Ctrl+B. There is no status line and no hint line while a terminal is shown (the
program owns the whole screen); one line with the keys is printed when matmux attaches, before the
screen. The list (`matmux --attach`) is a full-screen view that leaves the alternate screen before a
terminal is shown, so the scrollback stays. When the terminal is taken over by another client, the
connection is lost or the program ends, your terminal is put back (alternate screen left if the program
had it, mouse/paste/focus off, cursor shown) and a plain line says what happened; after a take-over or a
lost connection type Enter to take it back, `l` for the list or `d` to leave. The same happens on
SIGTERM/SIGHUP and, as far as possible, on a crash.

`matmux --rendered` (and `matterm --attach-tty`) keep the other way: matmux draws the terminal cell by
cell in the alternate screen, with the status line and `Ctrl+B s`/`?`; no host scrollback.

Known limits: the hidden screen does not know the tab stops, the character sets or the pen (the colours in
force for the next character) of the program, so right after the attach these are the host's until the
program sets them again; a program in the alternate screen: what was behind it on the primary screen is not
restored (the host shows what it showed before the attach).

Each row starts with the name of the terminal's top folder (`~/projects/matterm` shows `matterm`; `/` for the
root or an unknown folder), or the name it was given with `--name` / Rename terminal..., which wins; then
what runs (the command, or `shell`) and the full folder.

**Ending a terminal** from the list: `x` (or Delete) on a row asks in the note line `End "matterm" (vim)? y = end, n/Esc = keep` (it adds "also open in another client" when someone has it open); only `y` ends it, Enter, Escape or any other key keeps it. The terminal you run matmux in cannot be ended from its own list. After ending, the list is read again and the chosen row stays near. A daemon that is too old for this says so in the note line.

The list has **New terminal** as its first row (key `n`; your shell, in the folder you are in), then
the terminals of the daemon (name, what runs, `(in use)` if another client has it). Choose with
Up/Down (or `j`/`k`, `g`/`G`, or a digit) and Enter; choosing is the confirmation: another client
(the window on the computer) is kicked, as when two windows share a terminal, and says "press Enter
to take it back". The terminal this program itself runs in is marked `(this terminal)` and cannot
be chosen (attaching it would feed it to itself; the daemon tells each terminal its id in
`MATTERM_SESSION`).

| Where | Key | Does |
|---|---|---|
| list | Up/Down, `j`/`k`, `g`/`G`, 1..9 | choose |
| list | Enter | open the chosen row (`New terminal` starts one) |
| list | `n` / `r` | new terminal / read the list again |
| list | `x` or Delete | end the chosen terminal (and its program) after a question: `y` ends, anything else keeps it |
| list | `q`, Esc, Ctrl+C | quit |
| terminal | **Ctrl+B** then `d` | leave: the client ends, the terminal keeps running |
| terminal | Ctrl+B then `l` | back to the list (Ctrl+] does the same, as a spare) |
| terminal | Ctrl+B then `c` | a new terminal (this one keeps running) |
| terminal | Ctrl+B then `s` | the status line on or off |
| terminal | Ctrl+B then `?` | help |
| terminal | Ctrl+B Ctrl+B | send one Ctrl+B to the program |
| terminal, after being taken over or a lost connection | Enter | take it back / connect again |

Ctrl+B is the prefix of the window as well. After it a line at the bottom shows the possible keys
until the next key (which is not sent to the program). The status line (name of the terminal and the
main keys) is on by default from 24 rows and takes one row: the program has the screen minus that row
(the size sent to the daemon follows). When the program ends, the client says "the program in X ended
(exit code N)" and returns to the list; with `--new` it ends instead, with the program's exit code.

All other keys go to the program: arrows, function keys, Ctrl and Alt combinations, the mouse (only
while the program asked for it, so the phone's own selection works otherwise) and paste (in
brackets only if the program asked). Turning the phone or resizing the terminal resizes it.

If the connection drops (ssh ends), the terminal keeps running in the daemon. Log in again and run
the same command: choose it, and the screen is rebuilt from the daemon's replay (a full-screen
program such as `vim` or `htop` looks right).

Options: `--daemon-socket PATH` for another daemon; `--daemon-host [user@]host` from a computer
(not needed from the phone: you are already on the computer) to reach the daemon of a third one
through the tunnel of section c). The list does not start a daemon (with none running it says so);
`--new` starts one, or uses a compatible one.

Drawing follows the terminal's colours (truecolor, 256 or 16, from `COLORTERM`/`TERM`); the program's
own default and 0..15 colours come out as the terminal's theme. The mode lives in the SDL-free library
`matterm::attachtty` (`app/attachtty.h`).

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
