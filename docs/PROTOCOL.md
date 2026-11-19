# matterm-tty Wire Protocol, Version 1

Status: **specification for implementation, nothing implemented.** Rationale,
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
except `Hello`), `Closing` (nothing is accepted). Client states mirror this:
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

- `src/protocol/` provides, in C++17 and without I/O, one struct per message, a
  `std::variant` over each direction, `encode(message, std::string &out)`, and
  `decode(std::string_view)` returning the message or a `DecodeError`
  (`Truncated`, `Trailing`, `UnknownType`, `WrongDirection`, `BadString`,
  `TooLarge`, `OutOfRange`). Limits are checked before any allocation.
- The decoder is a pure function of one complete message, so it can be driven
  by a fuzzer and by round-trip tests for every message, plus the examples above
  as golden vectors.
- The state machine (`AwaitHello`, `Running`, `Closing`), timers, and flow
  control live in a transport-independent session class so they can be tested
  with a fake clock and an in-memory transport.
