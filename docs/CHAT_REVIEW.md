# Chat: security and robustness review

**Status: F1-F6 and the observations below are fixed or documented** (see
CHAT.md section 7, "Limits added after the review"); the six issue suites now run
as normal tests (`chat_fix_*`).

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

## 6. Load and soak

`test/chatd_soak_test.cpp` (CTest `chatd_soak`, label `soak`, about 35 s;
`tools/chatd-soak.sh [seconds]` runs each phase for longer) drives a chatd in
the same process over the plain TCP listener on loopback and measures memory and
descriptors from `/proc/self`:

| Phase | Load | Checked |
| --- | --- | --- |
| A | 100 chatting clients (drafts, locks, join history) with the room limits lifted | nobody dropped, about 65 000 lines delivered, all gone at the end |
| B | 10 clients that never read (1 KiB receive buffer) and 8 that talk at about 2000 lines per second | every non-reader is dropped by the backlog cap, no talker is lost |
| C | 5 flooders (20 lines per round, a quarter of them garbage) with the real limits, and a probe pair | every flooder is removed, probes are delivered (30 of 30, worst delay 3 ms) |
| D | one address connecting in a loop, with the real limits and with them lifted | with limits at most `connectBurst` plus the refill are admitted and nobody is banned; without limits all 680 visits are admitted and the room returns to its one resident |
| end | | peak resident growth under 64 MiB (measured 4 MiB), descriptors unchanged |

It found no defect in chatd. It does not cover TLS or the proxy, and with one
source address (loopback) it cannot show the per-source fairness across many
addresses (see F4 in section 2).

## 7. Deployment files (secondary review)

A read-through of `deploy/chatd/` against what chatd does (idle timeout 90 s with
keep-alive pings, 5 s handshake deadline, one request per connection, exact
Origin, X-Forwarded-For only from `--trust-proxy`) and against nginx and Caddy as
the reviewer knows them. **Nothing here was run**: no nginx, Caddy or Podman is
available, and no documentation was fetched, so every "correct" below is from
knowledge of those programs and is **unverified** until someone runs it
(`nginx -t` at least).

Correct as written (unverified in practice): `map $http_upgrade` with
`proxy_http_version 1.1` and the two `Upgrade`/`Connection` headers; the
`X-Forwarded-For $remote_addr` replacement (never `$proxy_add_x_forwarded_for`);
`Origin` passed through untouched; `proxy_read_timeout 120s` above chatd's pings
(they reach nginx every 45 s of silence); `limit_req` on the upgrade request
only (one per connection); `client_max_body_size 1k`; `access_log off`; the
Containerfile needs no libsodium (chatd-core has none) and `libboost-dev` is
enough because Asio and Beast are header-only; Caddy upgrades WebSocket by
itself and replaces X-Forwarded-For from an untrusted client.

Findings and suggestions (for the owner of the files). **Status: 1-11 applied to the example files (still unverified); 12 left as is.**

1. **nginx: no `ssl_protocols`.** Before nginx 1.23.4 the default includes TLS 1.0
   and 1.1; the matterm-server example sets `ssl_protocols TLSv1.2 TLSv1.3`.
   Add it here too.
2. **nginx: the comment "Only the WebSocket path exists; anything else is refused"
   is not what `location /` does.** nginx forwards everything; chatd refuses
   what is not an upgrade. Either say so or use `location = /` and `return 404`
   elsewhere.
3. **nginx: slow headers.** `client_header_timeout`/`client_body_timeout` default
   to 60 s and chatd's 5 s handshake deadline does not apply because nginx reads
   the request first. The matterm-server example sets 10 s; set it here.
4. **nginx and Caddy: IP addresses outlive `access_log off`.** The error log
   records the client address for refused requests (`limit_req`, `limit_conn`,
   bad requests; nginx `limit_req_log_level` defaults to `error`), and Caddy logs
   errors to stderr, which Podman/journald keep. Say so in the README and
   suggest `error_log ... crit;` (nginx) if the operator wants no addresses at
   all.
5. **nginx: `limit_conn 4` per address** is a shared-address trap (mobile
   carriers, universities, offices behind one NAT): five people in one office
   cannot all be in the room. Pick the number knowingly (chatd's own cap is 3
   per source, `maxPerSource`). IPv6: `$binary_remote_addr` is per /128, so one
   /64 owner gets 2^64 addresses; chatd groups by /64 but nginx does not
   (a `map` that keeps the /64 is the usual answer; unverified).
6. **nginx: `listen 443 ssl http2;`** is deprecated since nginx 1.25.1 (`http2 on;`).
   Harmless. Browsers still use HTTP/1.1 for the WebSocket because nginx does
   not offer the extended CONNECT of RFC 8441 (unverified).
7. **Caddy: no per-address limit and no timeouts.** Stock Caddy has no
   `limit_conn` (a plugin is needed), so only chatd's caps apply; and the default
   server timeouts for reading headers and the body are unset in the versions the
   reviewer knows. Suggest `request_body { max_size 1KB }` and a global
   `servers { timeouts { read_header 10s read_body 10s } }` (check against the
   installed version), and say that the rate limits then depend on chatd.
8. **compose.yaml: `${CHAT_PROXY_IP:-none}`** (see SECURITY_REVIEW.md, risk 2):
   require the variable (`:?`) or print a warning, because the room is then
   limited as if every visitor were one.
9. **compose.yaml: the address chatd sees.** Whether chatd sees the host side
   of the container network (the README's `10.89.0.1`), `127.0.0.1` or another
   address depends on the Podman version and its network backend (rootless
   Podman is the doubtful case). The README should tell the operator to check
   with the console's `list` (the source shown for the first visitor) before
   trusting it. Unverified.
10. **compose.yaml: `podman attach` and Ctrl-C.** `podman attach` proxies signals
    by default, so Ctrl-C from the operator's terminal stops chatd and the room
    with it. Document `--sig-proxy=false` or detaching with the detach keys
    only (unverified for the installed Podman).
11. **compose.yaml: outbound network.** chatd never connects out; an `internal`
    network would remove the route (published ports on an internal network are
    unverified, as for matterm-server). `image: mattrix-chatd` is an unqualified
    name; `localhost/mattrix-chatd` avoids a registry search.
12. **Containerfile:** the base image is a floating tag (`debian:12-slim`); pin a
    digest for repeatable builds if that matters. `COPY test`/`demo`/`bench` are
    needed only because the top CMakeLists adds them; a `.containerignore`
    would keep the context small. Static glibc linking works for chatd
    (numeric addresses only); a `getpwnam`-style warning would be the sign that
    something started to need NSS.
