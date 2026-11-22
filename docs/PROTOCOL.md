# matterm-tty Wire Protocol, Version 1

Status: the codec, baseline server state machine and single-session clients are
implemented. Sections 1–10 describe that baseline. Section 11 is an **approved,
implementation-in-progress** resume contract; codec vectors alone do not imply
semantic support. Rationale,
threat model, authentication, and deployment are in
[WEB_TERMINAL.md](WEB_TERMINAL.md); this file defines only the bytes.

The protocol connects a terminal emulator running in a client (the browser) to
a server that owns a PTY. It carries the same bytes a native terminal exchanges
with its PTY, plus a few control messages. The server never parses VT.

## 1. Transport

- A reliable, ordered, message-oriented channel. One *message* maps to one
  transport message; the protocol has no length prefix of its own.
- **WebSocket:** endpoint `GET /ws`, subprotocol `matterm-tty.v1` (offered by the
  client, echoed by the server, otherwise the upgrade is refused). Binary
  messages only; text messages close the connection. `permessage-deflate` is not
  negotiated.
- **WebRTC data channel (later):** one channel labeled `tty`, `ordered: true`,
  fully reliable. Same messages unchanged. The 16 KiB message limit below is
  chosen so it works over SCTP without fragmentation concerns.
- **Authentication** happens before the upgrade (HTTP login, session cookie,
  `Origin` check). Failures there are HTTP errors (401, 403, 429), never
  protocol messages. After a successful upgrade the connection is authenticated.

## 2. Encoding rules

- Every message starts with a **type byte**, followed by the payload described
  for that type. Client and server types are disjoint (`0x01..0x7f` and
  `0x81..0xff`), so a message sent in the wrong direction is detected.
- Integers are unsigned, **big-endian**, fixed width. `i32` is two's complement.
- `string`: `u16 length` followed by that many bytes of UTF-8 (no terminator).
- `bytes` at the end of a message: all remaining bytes of the message.
- A message must be consumed exactly. Missing bytes, trailing bytes after a
  fixed layout, a string that overruns the message, or invalid UTF-8 in a
  `string` is a protocol error (`BadMessage`).
- Maximum message size is **16384 bytes including the type byte**, in both
  directions. A larger message closes the connection (WebSocket close code 1009)
  before the payload is buffered.

## 3. Messages

### 3.1 Client to server

| Type | Name | Payload |
|---|---|---|
| `0x01` | `Hello` | `u16 version`, `u16 columns`, `u16 rows`, `u64 resumeOffset`, `string sessionId`, `u32 features` |
| `0x02` | `Input` | `bytes data` (1 to 16383 bytes) |
| `0x03` | `Resize` | `u16 columns`, `u16 rows`, `u16 pixelWidth`, `u16 pixelHeight` |
| `0x04` | `Ack` | `u64 consumedOffset` |
| `0x05` | `Ping` | `u32 nonce` |
| `0x06` | reserved (`Detach`) | not valid in v1; the server answers `BadMessage` |
| `0x07` | `Close` | none |

`Hello`
: First message, sent exactly once. `version` is the highest protocol version the
  client speaks (1). `columns`/`rows` are the initial terminal size, 1 to 1000.
  In v1 `resumeOffset` must be 0 and `sessionId` empty; anything else is answered
  with `ResumeUnsupported`. `features` is a bit set of optional extensions the
  client understands; v1 defines none, so it must be 0 in v1 clients, and a
  server ignores bits it does not know.

`Input`
: Bytes for the PTY master, exactly as `InputEncoder` produces them: UTF-8 text,
  control characters, escape sequences for keys, mouse reports, focus reports,
  bracketed paste, and the terminal's own replies (DA, cursor position, OSC color
  reports). The server does not interpret or filter them. Empty `Input` is a
  protocol error. Clients should split pastes into messages of 4 KiB or less.

`Resize`
: New terminal size, 1 to 1000 in each direction. The pixel size is the size of
  the whole text area in device pixels, or 0 if unknown (it fills `ws_xpixel` /
  `ws_ypixel` of `TIOCSWINSZ`). The server applies it with `TIOCSWINSZ`, which
  delivers `SIGWINCH` to the foreground process group. At most 10 resizes per
  second are applied; later ones within the window are coalesced to the latest.
  Resizes beyond a hard cap (100 per second) are `RateLimited`.

`Ack`
: Cumulative acknowledgement: the client has parsed all output bytes with stream
  offset below `consumedOffset`. It must never decrease and must never exceed the
  end of what the server has sent. See section 5.

`Ping`
: The server replies with `Pong` carrying the same `nonce`.

`Close`
: The client ends the session. The server sends `SIGHUP` to the PTY's process
  group, sends any remaining `Output`, then `Exit`, and closes (code 1000). The
  same happens if the transport simply drops, but without the final messages.

### 3.2 Server to client

| Type | Name | Payload |
|---|---|---|
| `0x81` | `Welcome` | `u16 version`, `string sessionId`, `u64 startOffset`, `u8 flags`, `u32 features` |
| `0x82` | `Output` | `u64 offset`, `bytes data` (1 to 16375 bytes) |
| `0x83` | `Exit` | `i32 code`, `i32 signal` |
| `0x84` | `Pong` | `u32 nonce` |
| `0x85` | `Error` | `u16 code`, `string reason` |

`Welcome`
: Reply to `Hello`. `version` is the chosen version (`min` of both sides). In v1
  `sessionId` identifies the session for logs and later resume, `startOffset` is
  0, and `flags` is 0 (bit 0 `resumed` and bit 1 `outputLost` are reserved for
  resume). `features` is the subset of the client's bits the server accepts.

`Output`
: Raw PTY output. `offset` is the position in the session's output stream of the
  first byte of `data`; the stream starts at 0 and has no gaps, so each message
  must start where the previous one ended. A client that sees a mismatch treats
  it as a protocol error and disconnects. Messages carry up to 16375 bytes
  (16384 minus the 9-byte header) and the server may coalesce PTY reads up to
  that size.

`Exit`
: The process has ended and all its output has been sent. `code` is the exit
  status, or -1 if it was killed by a signal; `signal` is the signal number, or 0
  if it exited normally. It is always the last message; the server then closes
  the transport (code 1000).

`Pong`
: Reply to `Ping`.

`Error`
: A fatal condition; the server sends it and then closes the transport. `reason`
  is human-readable, at most 256 bytes, and must not contain secrets. See the
  table in section 6.

## 4. Connection lifecycle

```
Client                                   Server
  |  HTTP POST /login (password)           |
  |--------------------------------------->|  rate limit, verify, Set-Cookie
  |  GET /ws  (cookie, Origin, subprotocol)|
  |--------------------------------------->|  check cookie + Origin
  |<---------------------------------------|  101 Switching Protocols
  |  Hello                                 |  (within 5 s, else close)
  |--------------------------------------->|  create session, start the program
  |<---------------------------------------|  Welcome
  |<=======================================|  Output ...   (until Exit)
  |=======================================>|  Input, Resize, Ack, Ping ...
  |<---------------------------------------|  Exit, then transport close
```

Server states: `AwaitHello` (only `Hello` is accepted), `Running` (everything
except `Hello`), `Closing` (after `Close`: later messages are ignored while the process ends). Client states mirror this:
`Hello` is sent immediately after the upgrade; `Input` and `Resize` may be sent
before `Welcome` arrives and are queued by the server. Any message that is not
valid in the current state is `BadMessage`.

Timeouts (all configurable): `Hello` within 5 s of the upgrade; the server
closes with `Timeout` if it has received no message at all (including `Ping`) for
60 s, so clients send a `Ping` every 20 s when idle; a client that does not
`Ack` while the window is full (section 5) for 30 s is disconnected.

In v1 a closed transport ends the session: the process group receives `SIGHUP`,
and after a short grace period `SIGKILL`.

## 5. Flow control

The stream has one counter per direction of interest, the **output offset**.

- The server keeps `sent` (offset after the last byte sent) and `acked` (the
  highest `Ack` received). It reads from the PTY master only while
  `sent - acked < window` (default 262144 bytes, configurable). When the window
  is full it stops reading; the kernel's PTY buffer then fills and blocks the
  child, which is the intended back-pressure.
- The client sends `Ack` whenever at least 16 KiB of parsed, unacknowledged
  output has accumulated, and also once per animation frame or 100 ms if any
  unacknowledged output remains, so the server cannot stall on a quiet tail. The
  window must be larger than the ack threshold plus one message.
- Client to server needs no ack: `Input` is small and human-paced. The server
  queues at most 1 MiB of pending PTY input; beyond that it sends
  `Error(LimitExceeded)` and closes.
- A client message rate limit is enforced to bound CPU: at most 1000 messages per
  second averaged over 5 seconds, else `RateLimited`.

## 6. Errors and close codes

| `Error` code | Name | Meaning | WebSocket close |
|---|---|---|---|
| 1 | `BadMessage` | Malformed, wrong direction, or not valid in this state | 1002 |
| 2 | `UnsupportedVersion` | No common protocol version | 1002 |
| 3 | `ResumeUnsupported` | `Hello` asked to resume a session (v1: never) | 1008 |
| 4 | `LimitExceeded` | Message or queue limit exceeded | 1009 |
| 5 | `Timeout` | Missing `Hello`, idle, or no `Ack` | 1008 |
| 6 | `Replaced` | Another client attached to this session (reserved) | 1008 |
| 7 | `ShuttingDown` | The server is stopping | 1001 |
| 8 | `RateLimited` | Too many messages or resizes | 1008 |
| 9 | `StartFailed` | The configured program could not be started | 1011 |
| 10 | `Internal` | Unexpected server error (details only in the server log) | 1011 |

Codes 11 and up are reserved. Receivers must treat an unknown code as fatal.
After `Exit` the close code is 1000. A client that loses the transport without
`Exit` or `Error` reports the session as disconnected.

## 7. Limits (defaults)

| Limit | Value |
|---|---|
| Message size, both directions | 16384 bytes including the type byte |
| `Input` payload | 1 to 16383 bytes |
| `Output` payload | 1 to 16375 bytes |
| Columns / rows | 1 to 1000 |
| `sessionId` / `reason` | at most 64 / 256 bytes |
| Output window | 262144 bytes |
| Pending input per session | 1 MiB |
| Time to first `Hello` | 5 s |
| Idle (no client message) | 60 s |
| Ack stall | 30 s |

Servers may use stricter limits; every number above is configuration, not a
protocol constant, except the 16 KiB message size, which is fixed in v1.

## 8. Versioning and extension

- `Hello.version` is the client's highest version, `Welcome.version` the chosen
  one. A server that supports none of it answers `UnsupportedVersion`.
- New optional behavior is negotiated with `features` bits. New message types
  take new type numbers and are sent only if the peer negotiated them; an
  unknown type from a peer is `BadMessage`.
- Planned additions: resume (`resumeOffset`, `sessionId`, `Welcome.flags`),
  multiple attached viewers, and a server-sent title or bell notification. None
  changes the existing layouts.

## 9. Worked examples

All values hexadecimal. `Hello` for an 80x24 terminal, new session:

```
01                         type Hello
00 01                      version 1
00 50   00 18              columns 80, rows 24
00 00 00 00 00 00 00 00    resumeOffset 0
00 00                      sessionId "" (length 0)
00 00 00 00                features 0
```

`Welcome` for session `ab12`, stream starting at 0:

```
81  00 01  00 04 61 62 31 32  00 00 00 00 00 00 00 00  00  00 00 00 00
type version  "ab12"           startOffset               flags  features
```

The user types `ls` and Enter (`Input`), and the server answers (`Output`):

```
02 6c 73 0d                                  Input "ls\r"
82 00 00 00 00 00 00 00 00 6c 73 0d 0a       Output offset 0: "ls\r\n"
82 00 00 00 00 00 00 00 04 61 2e 74 78 74 0d 0a   Output offset 4: "a.txt\r\n"
04 00 00 00 00 00 00 00 0b                   Ack 11
```

The browser window is resized to 120x40 cells (no pixel size known), then the
shell exits with status 0:

```
03 00 78 00 28 00 00 00 00                   Resize 120x40
83 00 00 00 00 00 00 00 00                   Exit code 0, signal 0
```

A mouse press in SGR encoding (the terminal enabled mode 1006) is an ordinary
`Input` whose data is `1b 5b 3c 30 3b 35 3b 33 4d` (`ESC [ < 0 ; 5 ; 3 M`).

## 10. Implementation notes

- `src/protocol/` (`matterm::protocol`) provides, in C++17 and without I/O, one struct per message, a
  `std::variant` over each direction, `encode(message, std::string &out)`, and
  `decode(std::string_view)` returning the message or a `DecodeError`
  (`Truncated`, `Trailing`, `UnknownType`, `WrongDirection`, `BadString`,
  `TooLarge`, `OutOfRange`). Limits are checked before any allocation.
- The decoder is a pure function of one complete message, so it can be driven
  by a fuzzer and by round-trip tests for every message, plus the examples above
  as golden vectors.
- The server-side state machine (`AwaitHello`, `Running`, `Closing`), timers, flow
  control, resize coalescing, and rate limits are implemented as
  `protocol::ServerSession` (`src/protocol/serversession.h`). It takes time as a
  parameter and talks to its environment through a `SessionHost` interface
  (send, close transport, start/resize/terminate process, write input), so the
  tests drive it with a fake host and no clock or sockets. The host reads at most
  `outputBudget()` bytes from the PTY at a time, calls `tick()` by `nextDeadline()`,
  and must report process exit only after the PTY output was drained.

## 11. Replay/attach extension (approved contract, implementation in progress)

Agreed with the integrator: use existing v1 layouts, select exclusively in Hello
and keep ids out of URLs. Codec tests alone are not proof of semantic support;
the baseline remains the fallback for feature-zero clients.

### 11.1 Upgrade selection and authentication

- Every terminal uses bare `GET /ws`; query selectors are not supported.
  Process creation/selection waits for Hello, as it already does today.
- An empty Hello.sessionId requests a new terminal. A non-empty id selects an
  existing one; ids are exactly 32 lowercase hex characters encoding 128 random
  bits generated by the server.
- Every upgrade requires the existing cookie, exact Origin/Host and subprotocol.
  The id is a selector, NOT an authentication credential. Validate authentication
  before looking up ids or exposing existence. No passwords/tokens in URLs.
- New-session Hello has an empty `sessionId` and zero `resumeOffset`. Attach
  Hello supplies the selected id within the existing Hello deadline. A failed
  selection never falls back to creating a shell.
- Authenticated unknown/expired/guessed ids all get `ResumeUnsupported` with the
  same generic reason `session unavailable` (close 1008). Unauthenticated
  requests get 401 without a registry lookup. Do not echo ids into errors.
- Ids appear only inside binary messages, never URLs or server logs. No nginx
  query-log sanitizing prerequisite exists. Server auth logs keep only source
  hashes, never terminal ids or output.

### 11.2 Negotiation, flags and absolute offsets

Reserve feature bit `0x00000001` as **Replay**. Unknown feature bits continue to
be ignored. A new connection may advertise it; the server echoes it only when
replay/persistence is enabled and available for that terminal. A non-empty
Hello.sessionId without successful Replay negotiation is ResumeUnsupported.
Legacy feature-zero terminals remain ephemeral even if the server enables
persistence for capable terminals.

`persistent_sessions=false` or `replay_buffer_bytes=0` disables persistence and
recording into the ring entirely. A new legacy terminal still works. Several
new terminals per login are allowed in both modes, subject to connection and
session limits; do not treat this as a lifetime count of upgrades per token.

Offsets count **raw PTY output bytes from process start**, not WebSocket headers,
replayed copies, timestamps, input, reset notifications or resize events. They
never restart at attach and never wrap: end the session before u64 overflow.

For a retained range `[oldest,end)`:

- On a new terminal, Welcome has startOffset=0, flags=0 and a fresh id.
- On attach with `oldest <= resumeOffset <= end` and nonzero resumeOffset,
  the client promises it retains the parser/model state after that byte prefix.
  Welcome.startOffset equals resumeOffset and flags=`0x01` (**resumed**).
- On attach with resumeOffset=0 the client requests a fresh reconstruction,
  rather than promising any old parser state. On a missing prefix (offset below
  oldest), reconstruct from the oldest available newline-aligned position and
  set flags=`0x03` (**resumed | outputLost/reset required**). If the entire
  retained prefix from zero is available, reconstruction starts at zero but
  still uses 0x03 to require resetting any old local state.
- An offset above end is BadMessage (never silently skip unproduced output).
- If no complete safe-start line remains, drop retained bytes and start at end
  with flags=0x03. The session is still alive; SIGWINCH may produce a redraw.
- First Output.offset equals Welcome.startOffset; subsequent messages are
  contiguous. Every Ack is an absolute offset in this same stream, initially
  acked=sent=startOffset. The usual output window and Ack-stall limits apply to
  replay and live output together. Do not read additional live output into a
  second unbounded queue while replay is blocked.

Only flag bits 0 (resumed) and 1 (outputLost/reset) are defined, and bit 1 requires
bit 0. A client must reject flags it cannot implement, or flags/features not
negotiated. Reserved bits remain layout-preserved by the codec, but are not
permission for a session state machine to silently ignore data loss.

For flag 0x03, ClientSession must notify a new frontend **reset/replay callback**
before any replay Output. The frontend resets its model/parser/selection. This
callback/API must be agreed with the integrator: a transport cannot restore
terminal state merely by accepting a nonzero offset. No reset escape bytes are
inserted by the server or charged to stream offsets. The existing feature-zero
browser needs no code change while persistence remains off.

### 11.3 Take-over, close and resize

After authenticated lookup and a valid matching Hello, attach takes over the
terminal atomically. The previous attachment receives
`Error(Replaced, "session taken over")` followed by WebSocket close 1008. Its
late ACK/Input/resize/close events must not affect the new attachment or kill
its process. Use an attachment-generation fence, not just a reusable socket
pointer. Failed authentication, invalid Hello or guessed ids must never evict
an existing attachment.

A transport loss detaches a capable persistent terminal; an explicit protocol
Close always kills it, wipes its replay and closes after the existing bounded
drain/Exit policy. Protocol violations also end the terminal (do not turn fatal
errors into a persistence bypass). Detached sessions have no connection idle or
Ack timer, but do have their configured detached timeout and bounded ring work.
If a connected peer stops ACKing, normal backpressure/timeout remains enforced.

Hello columns/rows are authoritative on attach; apply them to the PTY. Force a
SIGWINCH redraw even if dimensions match, using a one-cell toggle and restore
when necessary, without changing the protocol's resize limits. Replay is sent
before newly generated live output. No server VT parser or mode scanner is added.

### 11.4 Agreed lifecycle/ownership decisions

Approved choices (implementation below follows this contract):

1. The single owner account, rather than a particular cookie generation, owns
   terminal ids. A new successful login rotates the cookie and closes old
   attachments, but capable persistent terminals become detached and may be
   reattached with the new owner cookie. On authenticated logout, kill **all**
   terminals and wipe rings; a future password reload must do the same. Merely
   changing a credentials file does not reload it into a running server today.
2. `max_upgrades_per_login` counts concurrent authenticated transports, released
   on failed handshake/close, not all upgrades ever attempted with that token.
   A same-terminal take-over transfers that slot atomically. `max_sessions`
   counts live PTYs, attached or detached. `max_detached_sessions` separately
   bounds unattended PTYs; if detaching would exceed it, terminate that session
   rather than evicting another terminal. Detached timeout uses monotonic time
   since detachment and is not extended by rejected attempts.
3. On detached process exit, destroy the PTY and wipe output immediately. To
   report the original Exit on a later attach, retain bounded **metadata-only**
   exit tombstones (at most max_sessions, evict oldest; no output). An attach to
   a retained tombstone receives Welcome(startOffset=end, flags=0x03), then Exit
   and close. Otherwise it gets the generic unavailable error. This avoids
   keeping a dead shell's potentially secret output just to report its status.
4. Rings carry `(monotonic timestamp, bytes)` chunks, with per-session and total
   caps; evict oldest chunks globally first. Bound metadata/chunk allocations
   as well as payload (one-byte writes must not create unbounded object overhead),
   and wipe discarded storage, not merely string length, before reuse/free.
   Limit retained session count even with unlimited detached lifetime (`0`).

Agreed defaults: persistent_sessions=false, replay_buffer_bytes=0,
replay_total_bytes=64 MiB, max_sessions=16, max_detached_sessions=16,
max_upgrades_per_login=16 and detached_timeout_seconds=3600 (0 means never).
Expose an authenticated, strict-Origin/Host POST `/sessions/close-all` action
and POST `/logout` (the latter also revokes/clears the cookie). Neither is a GET
or accepts credentials/selectors in a query. Both kill/wipe all owner terminals
including detached entries. These HTTP actions require implementation alongside the registry, not a client
UI change.

A newline-aligned restart is only a best-effort raw-stream reconstruction, NOT
a VT checkpoint: newline can occur inside OSC/DCS, and prior alternate/mouse/
bracketed-paste modes can be lost. This accepted initial limitation must be
visible to the client and covered by tests; no claim of pixel-perfect resume or
security isolation of arbitrary replayed terminal programs is made. The optional
mode scanner in MULTI_TERMINAL remains an open follow-up, not part of this gate.

### 11.5 Worked extension bytes (all hexadecimal)

Id text `00112233445566778899aabbccddeeff` is 32 ASCII bytes (not 16 wire bytes):

```
30 30 31 31 32 32 33 33 34 34 35 35 36 36 37 37
38 38 39 39 61 61 62 62 63 63 64 64 65 65 66 66
```

Attach Hello, 80x24, resumeOffset=1234, Replay feature:

```
01 0001 0050 0018 00000000000004d2 0020
<the 32 id bytes above>
00000001
```

If the prefix at 1234 was evicted and a newline-aligned replay starts at 2048,
Welcome is:

```
81 0001 0020
<the same 32 id bytes>
0000000000000800 03 00000001
```

The next Output starts `82 0000000000000800 <data>`. A client that kept state
and can resume exactly at 1234 instead receives startOffset `00000000000004d2`
and flags `01`, then Output at 1234. Take-over closes the old attachment with:

```
85 0006 0012 73657373696f6e2074616b656e206f766572
```

This is Error code 6, a u16 reason length of 18, and `session taken over`, then
WebSocket close 1008. There is no extra channel/session-id prefix on Output.

## 12. Local daemon (Unix socket, additive)

`matterm-daemon` serves the same terminal protocol on a Unix socket instead of a WebSocket.
No network, no password: the socket is `$XDG_RUNTIME_DIR/matterm/daemon.sock` (directory 0700,
socket 0600; fallback `/tmp/matterm-<uid>/daemon.sock`), and the peer uid is checked with
`SO_PEERCRED`. Other users are closed without a word. `--daemon-socket PATH` selects another socket.

**Framing.** Each message is a 4-byte big-endian length (1..16384) followed by exactly the bytes a
WebSocket binary message would carry. A zero or larger length, or anything else that violates
this, closes the connection (no reply). There is no close code on this transport: the `Error`
message carries the reason and an orderly close is EOF. Reference: `src/daemon/framing.h`.

**Terminal connections.** The first frame is `Hello`; everything after is section 4–6 and 11
unchanged. The daemon offers feature bit `featureDaemon` (2) in `Welcome.features` to clients
that asked for it. Sessions persist across disconnects only when the client also asked for
`featureReplay`, and a detached session never expires; it ends when its process exits or on
Shutdown. The daemon runs a fixed command (the user's `$SHELL`), `Hello` cannot choose one.

**Control connections.** A connection whose first frame is `ControlHello` (0x40) is a control
connection: it has no session. Control types do not overlap the terminal types (client to daemon
0x40..0x44, daemon to client 0xc0..0xc4), so a terminal decoder rejects them as unknown.

| Type | Message | Fields |
| --- | --- | --- |
| 0x40 | ControlHello | u16 version (1) |
| 0x41 | ListSessions | – |
| 0x42 | SetShared | string id, u8 shared (0/1) |
| 0x43 | GetStatus | – |
| 0x44 | Shutdown | u32 expectedSessions |
| 0xc0 | ControlWelcome | u16 version, u32 pid, u32 features, string build |
| 0xc1 | SessionList | u16 n, n x (string id, u8 flags: 1 attached, 2 shared, u64 outputEnd) |
| 0xc2 | Status | u32 pid, sessions, attached, shared, clients; u64 uptimeMs; u32 idleExitSeconds |
| 0xc3 | ShutdownReply | u8 done, u32 sessions |
| 0xc4 | ControlError | u16 code (1 BadMessage, 2 NotControl, 3 UnknownSession, 4 Busy), string reason |

`ListSessions` and `SetShared` are answered with a `SessionList`. `Shutdown` runs only when
`expectedSessions` equals the number of live sessions at that moment: the client shows the owner
"this ends N sessions", and sends N only after the owner confirmed. If the count changed, the
reply is `done=0` with the real count and nothing ends. On `done=1` the daemon ends all
sessions, removes the socket and exits.

**Lifetime.** `<socket>.lock` is held with `flock` for the daemon's life, so a second daemon on
the same socket refuses to start; a socket file left by a crash is replaced. The daemon exits by
itself after `--idle-seconds` (default 60) with no session and no client. `--daemonize` detaches;
the parent returns when the socket is ready (or prints the error and exits 1).

**Control version 2** (additive; a version 1 daemon answers the new types with `ControlError`
BadMessage and closes). `ControlWelcome.version` tells which one runs.

| Type | Message | Fields |
| --- | --- | --- |
| 0x45 | CreateSession | string cwd, u16 columns, u16 rows |
| 0x46 | GetSessionInfo | string id |
| 0x47 | ExposedHello | u16 version |
| 0xc5 | SessionCreated | string id |
| 0xc6 | SessionInfo | string id, string cwd, u8 busy, u8 n, n x string argument |

`CreateSession` starts the user's shell (never a client-chosen command or environment) in `cwd`
when that is an absolute existing folder (anything else: the daemon's own folder) with the given
size, and answers `SessionCreated`; the client then attaches with `Hello{sessionId=id}` and
`featureReplay`. `ControlError` code 5 `StartFailed`: registry full or the shell did not start.
`GetSessionInfo` answers what the foreground process of the terminal is: `cwd` is its folder,
`busy` is true when it is not a shell and `argument` is its command line (only when it is
printable and bounded; see `frontend::probePane`). Errors: UnknownSession.

**Exposed connections** (`matterm-server --expose-local-sessions`). A connection whose first
frame is `ExposedHello` is answered with `ControlWelcome` and then limited to what a web
browser may reach: `ListSessions` returns shared sessions only, and a terminal `Hello` must
name a shared session (no session id would create one: refused). An unknown, a private and an
ended session are answered identically (`Error ResumeUnsupported "session unavailable"`), so
nothing leaks about private sessions. Any other control message gets `ControlError` and the
connection is closed. When the owner stops sharing a session (`SetShared false`) an exposed
connection attached to it receives `Error Replaced "sharing stopped"` and is closed; the session
itself keeps running.
