# Chat: security and robustness review

An independent review of the chat (`src/chat/`, `src/chatd/`, `app/chat/`) against
the design in [CHAT.md](CHAT.md). Method: read the code, write a threat model,
write negative and fuzz tests first, measure what they show. No production code
was changed. Six gaps were found; each has a test that fails today (suite
`chatissues_test`, registered as an expected failure, see "Tests"). Everything
else listed under "Holds" is covered by passing tests.

## 1. Threat model

Assets: another person's terminal (no foreign control sequences, no spoofed
output), the room's availability, the operator's console, the host (chatd must
not be a way in), and the visitors' IP addresses and names (nothing persists).

Trust boundaries: the **browser or terminal client** is hostile (anyone may speak
the protocol with any tool, so `Origin` is not authentication); the **server** is
trusted by the client only as far as its bytes are sanitized (a rogue or
intercepted server is in scope for the client); the **reverse proxy** is trusted
for TLS, request rates and `X-Forwarded-For` only when named by `--trust-proxy`;
the **operator console** is trusted.

| Threat | Defence | Result |
| --- | --- | --- |
| Escape or control sequence injected through text, names, titles, errors | strict UTF-8, allowed-character set, `sanitizeIncoming`, view composes all sequences itself | **Holds** (4000 random hostile frames, every output byte checked) |
| Name spoofing: Unicode homoglyphs, bidi, zero width | names are ASCII `[A-Za-z0-9_-]` | **Holds** for Unicode; ASCII look-alikes get through: finding F3 |
| Impersonating the operator or system | reserved list (`server`, `admin`, `mod*`, ...) | exact names only: F3 |
| Message or draft flood | token bucket per connection, draft flood limit, room-wide cap | the cap itself is a lever: F4 |
| Large lines, binary, fragmented frames | 1 KiB `read_message_max`, text only, no deflate | **Holds** (fragments are bounded as a whole) |
| Slow clients, slowloris | 5 s handshake deadline (absolute), 5 s to say hello, ping/pong timeouts, backlog cap | **Holds** for slow *senders*; handshake slots are shared: F2; the backlog cap breaks joining: F1 |
| Connection exhaustion | per-source and total caps, `maxPending` | per source only after the handshake: F2; no rate limit on connecting: F6 |
| Origin bypass | exact match of exactly one `Origin` header | **Holds** (14 disguises, a duplicate header) |
| Proxy header spoofing | `X-Forwarded-For` only from the named peer | **Holds** |
| `--test-tcp` reachable from outside | loopback addresses only, off by default | **Holds** (16 non-loopback spellings refused) |
| Memory growth | fixed ring, fixed tables, client caps | the ban table is unbounded: F5 |
| Console injection | names and source keys are validated | **Holds** |
| Log leakage | counters only, nothing logged | **Holds** (no message text, name or address is written anywhere) |

## 2. Findings

Severity is for a public room behind the planned proxy. Each finding names the
test that reproduces it; run `ctest -R chat_issue` (expected failures) or the
binary directly: `build/test/chatissues_test ChatIssue<Name>`.

### F1. A full history keeps every newcomer out (high)

`ChatServer::Impl::apply` queues a joiner's whole history (up to 200 `hist`
lines of about 230 bytes) before the first write completes, and `enqueue()`
reports the queue length to `ChatRoom::backlog`, which drops a connection over
`maxBacklogBytes` (16 KiB). About 70 messages of 200 characters are enough:
from then on **every later join is disconnected**, until the messages expire
(45 minutes). The cost to an attacker is a minute of messages from a few
connections. A control run with `maxBacklogBytes` at 1 MiB passes, so the cause
is the limit, not the history.

Reproduce: `ChatIssueJoinBurst` (`ctest -R chat_issue_JoinBurst`).
Fix: make the limit at least the worst-case history (`ringSlots` times about 260
bytes, so 64 KiB with margin), or do not count the join burst, or write the
history paced. Add a worst-case-history join to the passing tests.

### F2. One address can use up the pre-handshake budget (medium)

`maxPending` (128) is global and the per-source caps apply only after the
handshake, so one address holding 128 silent sockets (each dropped after 5 s,
then replaced) makes `acceptWs` close everyone else's connection. A TLS proxy
normally absorbs this; chatd is the last line only if it is reachable directly.

Reproduce: `ChatIssuePendingBudget` (a second source, 127.0.0.2, is refused).
Fix: a small per-peer-address cap on pending handshakes (for example 8), counted
in `acceptWs`/`acceptTcp` before the session exists.

### F3. ASCII look-alike names (medium)

Names are plain ASCII, so Unicode homoglyphs are impossible, but `Iike` (capital
i) sits next to `like`, `b0b` next to `bob`, and `adm1n`, `admln`, `adrnin`,
`serv3r` are not the reserved `admin` and `server`. The reserved list is the
defence against someone posing as the operator; it matches exact names only.

Reproduce: `ChatIssueLookAlikeNames`.
Fix: compare a *skeleton* in `findByName` and the reserved check: lower case,
`0`->`o`, `1`/`i`->`l`, `5`->`s`, `3`->`e`, `rn`->`m`, drop `_` and `-`. Expect a
few false positives among real names; refuse with the existing `name-taken` or
`bad-name`.

### F4. A few spammers take the whole message budget, and get others banned (high)

The room-wide cap (120 per minute) is a lever. Five connections on two addresses,
each posting at the per-connection rate (one per two seconds) reach 150 per
minute. Everyone else is then told `rate` every time. Worse, an explicit `lock`
refused for the room's cap counts as **that person's own mistake**, so five tries
get an innocent person kicked and banned for five minutes. Measured in a
simulation: 0 of 12 messages got through, and the newcomer was kicked. The
attackers use drafts only (the server locks for them), so no violation counts
against them.

Reproduce: `ChatIssueRoomBudget`.
Fix: do not count refusals caused by the room's cap as violations (a separate
`busy` error); share the cap fairly between sources (per-source share, or serve
the source that posted least recently), and make the per-connection token bucket
a per-source one so that three connections are not three times the rate. The
room cap can stay as a last brake. The same shape exists for the password budget
in `matterm-server` (docs/SERVER.md).

### F5. The ban table has no bound (medium/low)

`bans_` (and `mutes_`) grow with every banned source and shrink only by expiry
(five minutes after a kick, an hour for an operator ban). Five bad lines ban a
source, so an attacker with many addresses (an IPv6 /48 has 65536 /64 keys)
grows it at their own request rate. 6000 bans are held in the test.

Reproduce: `ChatIssueBanTable`.
Fix: cap both tables (for example 4096) and evict the entry that expires first;
an attacker then only rotates which of their addresses are banned, which costs
them as much as before.

### F6. No limit on how often one address connects (medium)

Only concurrent connections are capped. One address can connect, say hello and
leave in a loop: each round sends the newcomer the whole history (up to 46 KB)
and every other person a `join`, a `leave` and a changed count. A small request
costs the server and 64 clients a great deal. In the test 1000 rounds in ten
seconds are all admitted.

Reproduce: `ChatIssueReconnectChurn`.
Fix: a per-source token bucket on `connected()` (for example 10 per minute, burst
5) that refuses with `rate` before any history is sent.

## 3. Holds (and what shows it)

`chat_security_test` (core) and `chatd_security_test` (over real sockets):

- A hostile server cannot put its own sequences on the screen: 4000 random
  frames of every kind (`msg`, `draft`, `hist`, `notice`, `error`, unknown lines)
  with ESC, C1 in both encodings, OSC/DCS, bidi, zero width, tag characters,
  NUL, broken UTF-8; every byte written is checked against what the view
  composes (CSI with numeric parameters and the finals it uses, text, CR, LF).
- Pasted text is filtered; broken UTF-8 makes the whole paste be refused.
- The room never repeats client text in an error; errors are a few fixed lines.
- Non-ASCII names never get in (Cyrillic, full-width, combining marks, zero-width, bidi, NUL, controls, separators, path and shell characters).
- A flooding server cannot grow the client (users, typers, history, events).
- Only the exact `Origin` passes (case, port, scheme, path, `@`, suffix and prefix
  tricks, `null`, `*`, two headers). A space around the value is not a disguise:
  HTTP drops it and the value is exact.
- `X-Forwarded-For` from an untrusted peer never makes a new source.
- The plain TCP test listener is off by default and accepts loopback addresses
  only, in every spelling tried (`0.0.0.0`, `::`, `::ffff:10.0.0.1`, `localhost`,
  trailing space, ...).
- Silent and trickling requests are dropped on a fixed deadline; oversized
  headers, bodies, other methods, other paths and garbage are refused.
- Fragmented messages are bounded as a whole; a ping flood is cheap.
- Random garbage over 60 connections leaves the server and the bystanders
  sound.
- The operator console shows only valid names and addresses.

## 4. Observations (no change needed, or a note)

- Fragmented text messages that fit are assembled by Beast and accepted;
  CHAT.md section 3 says they are rejected. Harmless (the total is bounded by
  1 KiB): correct the text, or enforce it.
- The plain TCP listener has no idle timeout. Loopback only and for tests.
- A muted person gets no sign that their text is dropped (by design:
  "types into the void").
- Rate limits are per connection, mutes and bans per source. Behind a proxy
  without `--trust-proxy` every client shares the proxy's address, so the
  per-source cap and any ban would hit everyone. The server logs nothing to warn
  about it; worth a line in the operator notes.
- The client, `mattrix-chat`, is already bounded (2 KiB read buffers, message
  limit 2048, caps on users, typers, history, events, bad lines).
- Unverified, as in CHAT.md: the Podman hardening, TLS, a browser client.

## 5. Tests

| File | What | Registered as |
| --- | --- | --- |
| `test/chat_security_test.cpp` | core threat checks (hostile server fuzz, paste, errors, names, client bounds) | part of `chat` |
| `test/chatd_security_test.cpp` | server threat checks over loopback sockets, one fuzz | `chatd_security` |
| `test/chatissues_test.cpp` | one suite per finding F1-F6, each fails today | `chat_issue_<Name>`, `WILL_FAIL`, label `known-issue` |
| `test/chatdfixture.h` | shared fixture (a chatd on its own thread, a line client, a WebSocket client with headers and a source address) | |

A `WILL_FAIL` test passes while the check still fails and turns **red when the
code is fixed**. That is the cue to move the suite into `chatd_security_test.cpp`
or `chat_security_test.cpp` and delete its `chat_issue_` entry in
`test/CMakeLists.txt`.
