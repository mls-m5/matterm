# Chat in Mattrix OS (plan, nothing is built yet)

Status: **decisions recorded; the server, the client core, the terminal view and
`mattrix-chat` are built and tested locally** (section 12). Not built yet: the web
bridge and the `chat` command in Mattrix OS (nothing about chat is shown on the
Pages demo until a server address is configured), the Podman image and TLS in the
native client. Unverified points are marked **UNVERIFIED**. Ergo (IRC) remains a
fallback and its checked settings are kept in Appendix A.

## 0. Decisions so far

| Topic | Decision |
|---|---|
| Server | Our own `ChatRoom` core (pure C++, no I/O) plus a thin Boost.Beast layer (`chatd`). No Ergo, **no WebRTC/STUN**. |
| Storage | A ring buffer in RAM only; nothing is written to disk; a restart empties the room. Messages also **expire after a while** (TTL, proposed 45 min). |
| Accounts | None. A client sends a name and lines. Names are unique among live connections. |
| TLS | Terminated by the reverse proxy (`wss://` from the browser). Locally everything is plain `ws://`. TLS in a native client comes much later. |
| Hosting | Own machine, **Podman from the start** (section 7). Several sites may use the one shared room (one `chatd`, an Origin list with several entries). |
| Moderation | From the server's own terminal first, no accounts. Silent automatic defences; **no "no moderator here" notice** (it invites abuse). |
| Online count | Always visible ("12 here"). |
| Protocol | A minimal **line format**, not JSON (section 3). |
| Live typing | Drafts are sent while typing; everyone sees them; **cannot be turned off** (it is the point of the chat). A bubble **locks by itself after 5 s of stillness**, no Enter needed (section 4). |
| Looks | Bubbles with a title row, one cell of padding each side, blank line between bubbles, up to **6** live bubbles (section 5). |
| Mode | **Inline by default** (normal screen, the terminal's own scrollback); `--fullscreen` optional (section 5). |
| Text allowed | ASCII plus Latin letters (å ä ö, accents) at first; no emoji or wide characters. |
| Numbers (proposals) | Ring 200 messages, text 200 characters, name 2-16, 64 connections in total, 2-3 per IP, one message per 2 s with burst 3. |
| Room rules | Written in English. |
| Later | A cheap always-online bot (just another client); a chat-only web view; the separate `mattrix-chat` program. |

## 1. Goals and non-goals

* One public room, short plain-text messages, guests with a chosen name.
* The server keeps text only in a fixed-size RAM ring and a fixed table of live
  drafts; no logs of message content.
* Strict input checks in independent layers (section 6), so hostile text cannot
  reach a terminal, a file or another program.
* Not: accounts, private messages, several rooms, history search, file
  transfer, federation, IRC compatibility, peer-to-peer (WebRTC would show every
  participant's IP to every other, still needs a signalling server, often needs
  TURN, and cannot be moderated).

## 2. Architecture

```
 fakeos `chat` program ----+                      +-- server terminal (admin)
 mattrix-chat (any terminal)+--wss--> nginx/Caddy --ws--> chatd (Podman)
 web demo (JS WebSocket) ---+   (TLS, limits)            |  ChatRoom (pure core)
                                                          |  Beast adapter (I/O only)
```

| Piece | What | I/O? |
|---|---|---|
| `ChatRoom` | All server logic as a state machine: connections, names, validation, rate limits, caps, the draft table, the ring, moderator commands. Time injected. Events in (`connected`, `line`, `closed`, `tick`, `admin`), actions out (send, close with reason). | none |
| `chatd` | Boost.Beast server: accepts, forwards bytes and time to `ChatRoom`, executes its actions, reads the admin console. Listens on loopback; honors `X-Forwarded-For` only from the proxy (reuse `proxysource`, `sourceKey`). | yes, thin |
| `ChatClient` | Client state machine: Off, Connecting, Hello, Joined, Closed/Error; owns the local bubbles, the idle timer, the `not sent` logic; sanitizes everything it receives. Time injected. | none |
| Chat channel | Abstract connection (`send(line)`, `onLine`, `onClose`, `close()`); scripted test doubles; real: JS `WebSocket` (web), Beast client later (native). | per host |
| Chat screen | The inline "live region" renderer and the optional fullscreen view (section 5). | none |

The pure pieces live in a library with no sockets (like fakeos, with the include
check), so they also run in the web build. `matterm-server`'s `RateLimiter`
(password attempts) and `ReplayRing` (terminal bytes by serial and byte budget)
are **not** reused; chat needs a per-connection token bucket and fixed slots.

## 3. Protocol (line format)

One **WebSocket text frame = one line**, UTF-8, fields separated by a single TAB,
at most **1 KiB** per frame (in practice under about 260 bytes). Binary frames,
permessage-deflate, unknown commands and extra fields are rejected (Beast:
`read_message_max`, no deflate, `got_text()`). A text message that arrives in
fragments is assembled by Beast and accepted if the whole is within the limit;
the bound is on the whole message, not on the fragments. No JSON parser.
`B` is a **bubble number** the client increments per bubble (a small integer).

Client to server:
* `hello<TAB>NAME`: first line, within 5 s.
* `draft<TAB>B<TAB>TEXT`: the **whole** current draft of bubble `B` (0..200
  characters). An empty text withdraws the bubble. At most about 3 per second are
  used; the client coalesces.
* `lock<TAB>B`: the client's idle timer fired (5 s without a change): publish.
* `nick<TAB>NAME`: rename.

Server to client:
* `welcome<TAB>NAME`, one or more `users<TAB>a,b,c` lines (a long list is split to stay inside the frame limit), then `hist<TAB>SERIAL<TAB>AGE_S<TAB>NAME<TAB>TEXT` per kept message, then `ready`.
* `draft<TAB>NAME<TAB>TEXT`, `gone<TAB>NAME` (someone else's live bubble; coalesced, only the 6 most recently active typers are forwarded, the rest are counted: `typers<TAB>N`).
* `msg<TAB>SERIAL<TAB>NAME<TAB>TEXT`: a locked message (for everyone, also the author).
* `ack<TAB>B<TAB>SERIAL<TAB>TEXT`: to the author only: bubble `B` was locked as `TEXT`.
* `online<TAB>N` (at most about once a second), `join<TAB>NAME`, `leave<TAB>NAME`,
  `hide<TAB>SERIAL` (a moderator removed a message), `notice<TAB>TEXT`,
  `error<TAB>CODE<TAB>TEXT` (codes `name-taken`, `bad-name`, `too-long`, `rate`, `full`,
  `banned`, `bad-line`), usually followed by a close.

Rules for `B`: the server remembers the current `B` per connection. A `draft` or
`lock` with an **older** `B` is ignored (late or duplicate); a `lock` for a bubble
that was already locked or is empty does nothing; a `draft` with a **new** `B`
first locks the previous bubble (if it has text) and starts a new one. So sending
the same thing twice can never publish it twice.

**Locking.** The client locks after 5 s without a change (`lock`); the server
has the same rule as a backstop after about **6.5 s** (both are constants). An
empty or space-only bubble never locks and just disappears. Locking counts as
one message for the token bucket. Text is **trimmed** at lock time (leading and
trailing spaces are allowed while typing).

**When the server is first.** If `ack` arrives while the client has typed more,
the client removes the acknowledged text **from the start** of its input bubble
if it begins with it and keeps the rest as the new bubble. If the user edited
inside the locked part meanwhile, the client keeps its text unchanged (a
duplicate is better than lost typing).

**Not sent.** If the connection drops or an `error` arrives, the own bubble shows
`not sent` and stays until the user sends or clears it (no silent resend).

## 4. Live typing

* The server holds **one draft per connection**, at most 200 characters (about 13
  KiB for 64 connections); the validator bounds it, and the ring has a fixed
  number of slots. Drafts never enter the ring until locked.
* Traffic: each connection's drafts are limited to about 3 per second (extra ones
  are coalesced silently, a flood of more than about 10 per second is a
  violation); only changed drafts are forwarded; at most 6 typers are forwarded
  to each client, so the worst case at 64 users is a few hundred KB/s. UNVERIFIED
  until measured.
* **Privacy:** everyone sees what you type, including what you delete, and a
  pause of 5 s publishes it. This is the point, and the notice says it
  (section 8). Drafts cannot be switched off.
* Validation of a draft is the same as of a message except for leading/trailing
  spaces (trimmed at lock); a rejected draft is an `error`, never silently
  repaired.
* The client shows the author's bubble as `draft sent` while it is being sent
  live and `sent` after `ack`, so it is clear that typing goes out at once.

### How other people's live text is shown

Drafts arrive as whole snapshots (about three a second), but they are shown as if
they were being typed here: **one character at a time**. The rule (pure logic in
`ChatClient`, driven by the injected clock, deterministic for a given seed):

* Per person the client keeps the text **shown** and the latest **target**. A step
  either removes the last shown character (when the shown text is not a prefix of
  the target: a backspace or an edit in the middle) or appends the next one.
* The characters of a new snapshot are spread over the time since the previous
  snapshot (`gap / steps`), but each step takes **at least 25 ms and at most
  120 ms**, with a little seeded unevenness (plus or minus 20 percent). The first
  character of a burst appears at once.
* The display never lags more than **1.5 s**: when the backlog would take longer,
  the steps are made shorter than the floor.
* When the text is **posted** (`msg`), the rest is typed at 10 ms a character and
  only then does the finished bubble appear. Messages keep their order: later ones
  wait behind it. `gone`, which the room sends right after `msg`, does not cut the
  catch-up short; a moderator's `hide` removes a message that is still waiting; a
  closed connection shows what was waiting at once.
* Constants are `ChatClient::Config::{minCharMs, maxCharMs, maxLagMs, finalCharMs,
  jitterPercent, jitterSeed}`; tests: `test/chat_livetext_test.cpp`.

The own caret and the own bubble are not delayed; only what arrives from others.

## 5. The chat screen

### Look

Bubbles have a **title row** (a color band), one cell of padding left and
right, text, and a **blank row between bubbles**. A bubble is as wide as its
content (title or longest line, plus 2), up to a maximum (about 70% of the width);
longer text wraps. Finished and live bubbles differ by background shade. Own
bubbles are marked by a small face in the title row (`●`, or `:)` when the
terminal is not Unicode); all bubbles are left-aligned. In this sketch `▒` is the
title row's background and `░` the body's (not drawn in the real thing):

```
▒bo  14:02  sent▒▒▒▒▒▒▒▒▒▒▒▒▒
░hi everyone, anyone around?░

▒cy  14:02  sent▒▒▒▒
░hej! just got here░

 · dee joined

▒ada  14:05  sent▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
░anyone seen the crumble game? I think░
░it is hidden in /usr/games░░░░░░░░░░░░

▒bo  typing…▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
░i think it has a dot in fr▌░

▒cy  typing…▒
░ok but wh▌░░

▒● mia  draft sent▒▒▒▒▒
░what is the dot for▌░
```

* Title rows: `name  14:06  sent` (locked), `name  typing…` (someone else's live
  bubble), `● name  draft sent` / `sent` / `not sent` (own). Name in bold, time
  and state dimmer.
* **The live marker.** Another person's text that is still being typed ends in a
  filled left-pointing triangle, `◀` (U+25C0, constant `chat::kLiveMarker`; a plain
  `<` without Unicode, `kLiveMarkerAscii`), in an accent color that pulses slowly
  between bright and dim. It is not on your own bubble (that has the terminal's
  caret) and it goes when the message is posted.
* When a bubble locks it flashes briefly, becomes a locked bubble (time and
  `sent` appear, the shade changes) and a new empty bubble opens for the author.
* More typers than six: "+3 typing…". The count of people online is in a status
  line ("Mattrix chat · 12 here").
* The first lines say what this is: **"Live chat, draft sent as you type"**,
  pulsing for a few seconds and then written as a plain static line.
* Animations (cheap, optional): a smooth step when a line is added, a blinking
  caret in live bubbles, moving dots in `typing…` when a bubble is still empty,
  names fading in and out on join/leave. They never carry information and are
  switched off by `--plain`, by a setting and on slow terminals.

### Inline by default

* **Finished bubbles are printed once to the normal screen and become the
  terminal's own scrollback.** Only the **live region** at the bottom (the live
  bubbles, your own bubble, the status line) is redrawn in place with relative
  cursor movement. To add a finished bubble: move to the top of the live region,
  erase downwards, print the bubble as ordinary lines, redraw the region below.
* Your **cursor sits in your own bubble**, always the last one; there is no
  separate input line. If you have scrolled back and type, the view goes to the
  bottom (that is the terminal's behavior when output arrives or you type).
* Benefits: native scroll, search and copy, the same in matterm, `mattrix` and
  the web demo, no alt-screen. `--fullscreen` can add an alt-screen view later.
* Costs: animations only happen inside the live region (printed lines are
  final); a window **resize** reflows printed lines and the live region's row
  count may be wrong, so on resize the renderer clears from a remembered anchor
  and redraws (the known weak spot of inline UIs); it needs a new small
  **live-region renderer** (a pure function from region lines to bytes using
  CUU/EL/ED), not the full-screen `AsciiCanvas` flush. **UNVERIFIED** until built
  and tried in matterm, a plain terminal and the web demo.
* Inside Mattrix OS the same renderer works on the shell's primary screen, so
  `chat` behaves like any long-running command.

### Input

Word keys (also at the Mattrix OS prompt): Ctrl-W, Alt+Backspace and Ctrl+Backspace
(terminals send it as Ctrl-W) delete a word back; Alt+D and Ctrl+Delete a word
forward; Ctrl+Left/Right and Alt+B/Alt+F move by words.

One-line editor (Backspace, Ctrl-U/W/A/E, arrows), 200 characters with a visible
counter, paste is sanitized and truncated. Commands: `/help`, `/nick NAME`,
`/clear`, `/who`, `/quit` (Ctrl-C leaves at once). Anything else starting with
`/` is an error line and is not sent.

## 6. Strict input, in layers

1. **Proxy:** request size, connection and request rates, timeouts.
2. **Transport (Beast):** text frames only, 1 KiB cap, no compression, no
   fragmentation, ping/pong with timeouts (slowloris), handshake deadline.
3. **Server validation (`ChatRoom`)**, rejecting rather than repairing:
   strict UTF-8; **name** `[A-Za-z0-9_-]{2,16}`, unique among live connections, a
   small reserved list (`server`, `admin`, `system`, `mod*`, `you`); **text**
   0..200 code points from ASCII and Latin letters only: no C0/C1 controls (also
   not TAB, CR, LF), no DEL, no format/bidi/zero-width characters, no private-use
   or unassigned code points, no run of more than N identical characters, not
   only spaces; fixed commands and field counts.
4. **Limits (`ChatRoom`):** token bucket per connection (one message per 2 s,
   burst 3), per source (`sourceKey`), a hard total of connections (64), a
   per-source cap (2-3), a cap on connections still without `hello`, a cap on
   bytes queued per connection (a slow reader is disconnected, never buffered),
   draft rate limits (section 4), repeated violations disconnect and ban for a
   short time.
5. **Storage:** a ring of fixed slots in one preallocated array plus the draft
   table; nothing grows; the oldest is dropped, and entries expire after the TTL.
   **No file writes and no message text in logs** (counters only); the container
   is read-only with no volume (section 7). Names and IPs are not kept after a
   connection ends (active bans excepted, in RAM).
6. **Client (`ChatClient`):** treats everything received as hostile although the
   server checked it: strict UTF-8, removes C0/C1 controls (so no CSI/OSC/DCS can
   form; a leftover `[31m` is plain text), format bytes, bidi and zero-width
   characters, caps lengths and counts. One shared validator function for the
   server and the client.
7. **Screen:** everything drawn is cells or text the renderer itself composed;
   chat text never goes into the output as raw strings from the network, and
   never through the shell's `io.out`.
8. **Links:** URLs in chat are plain text; the demo's link detection must not
   make them clickable without a confirmation.

The shared validator is the single point to fuzz hardest.

## 7. Server, operation and Podman

* **Files:** `deploy/chatd/` (Containerfile, compose.yaml, nginx and Caddy
  examples, README). A fully static `chatd` links and runs (checked here with
  `-DMATTERM_CHATD_STATIC=ON`: 4 MB, "not a dynamic executable"); the image, the
  compose file and the proxy files are **unverified** (Podman cannot start in the
  development sandbox).
* **Container (UNVERIFIED, to be tried):** `chatd` as a statically linked binary
  in an image with nothing else (no shell, no other programs), run rootless with
  `--read-only`, `--cap-drop=all`, `--security-opt no-new-privileges`, memory
  and process limits, **no volume**, no outbound network, and only its one
  port published on loopback; nginx/Caddy in front does TLS and rate limits and
  forwards `Origin` and `X-Forwarded-For`. Whether Boost.Beast links statically
  here cleanly is to be checked.
* **Several sites, one room:** the Origin allowlist has one entry per site (the
  real pages, plus a local development origin); a connection without an allowed
  `Origin` is refused. Origin is a browser-enforced header, not authentication:
  it stops other *websites* using visitors' browsers, nothing more.
* **Admin console** on the server's terminal, no accounts (whoever can reach the
  host is admin): over stdin (`podman attach`/`run -i`) or a small Unix-socket
  console with a client in the image. Commands: `list` (name, age, source), `kick
  NAME`, `ban IP|NAME [minutes]`, `unban`, `mute NAME [minutes]`, `delete N`
  (remove a message and send `hide`), `clear`, `lock`/`unlock` (no new joins),
  `say TEXT` (a notice), `stats`, `quit`. Bans and mutes live in RAM and expire.
* **Unattended:** silent defences only: automatic kick and a short ban after
  repeated violations, a global cap on messages per minute for the whole room,
  and the remote `lock` as an emergency brake. The room never announces that no
  moderator is present.
* **Limits added after the review** (docs/CHAT_REVIEW.md): the join history
  fits the backlog limit (64 KiB); the message rate is one bucket per *source*
  (three connections are not three times the rate) and hitting the room's cap is
  nobody's violation; new connections per source are rate limited (burst 5, one
  per 6 s); at most 4 not-yet-handshaken sockets per peer address (not for the
  trusted proxy); bans, mutes and the buckets are bounded (4096, the entry that
  ends first is evicted); names are compared by *skeleton* (`l/I/1`, `0/O`,
  `5/S`, `3/E`, `rn/m`, `vv/w`, `_` and `-` folded), also against the reserved
  names.
* **Behind a proxy always set `--trust-proxy`.** Without it every visitor has the
  proxy's address, so the per-source limits, bans and mutes count everybody
  together. The `--test-tcp` listener (loopback, tests only) has no idle timeout.
* **Secrets:** none in the repo; configuration is the listen address, the
  Origin list and the numeric limits; the repo has a template with placeholders.
* The proxy's access log is where visitor IPs live; keep it short-lived.

## 8. The notice and the separate program

**Notice**, shown before any connection (`chat`, `mattrix-chat`; `?cmd=chat` never
connects without it): "**Live chat, draft sent as you type.** This connects to
<server> over a secure WebSocket. The server sees your IP address; other users
see only your name. Everyone sees what you type while you type it, and a pause of
5 seconds posts it. Messages are public, kept only in server memory for a short
time, and gone when it restarts. Rules: (3 short lines, English)." Off by default.

**Separate program** (later): `mattrix-chat`, started from a terminal, from
matterm or from inside Mattrix OS. Shared library; the chat screen and
`ChatClient` are the same in every host; the host part of `app/demo/ttyhost.cpp`
is split into a small "program host" that can run a chat program and also wait on
a socket. Native TLS needs OpenSSL (`libssl-dev` is **not installed in this
sandbox**; an apt request would be needed) and is postponed: web goes through the
proxy's TLS, and native uses `ws://` to a local `chatd` while developing. Starting
it from inside Mattrix OS (host-provided channel versus a host hook that launches
the program) is decided later; the fakeos core opens no sockets. A chat-only web
view and an always-online bot are later too.

## 9. Build order

Done (each with tests; see the commits): 1. the validator (`chattext`), 2. the
`ChatRoom` core, 3. the line codec (`chatproto`), 4. `ChatClient`, 5. the view
(bubbles, editor, inline live region), 6. `chatd` (Beast WebSocket with origin
allowlist, plus the loopback-only TCP test listener, operator console), 7.
`mattrix-chat` (TCP and plain WebSocket) with pseudo-terminal tests against a real
`chatd`. Next, in this order: 8. the web bridge and the hidden `chat` command in
fakeos (off until a server address is configured), 9. the Podman image and the
proxy snippet, with measurements of draft traffic, 10. TLS in the native client.

## 10. Still open

### Questions for the owner (decisions only you can make)

1. **Where does it run, and under which domain?** (`chatd` needs an address, a
   TLS-terminating proxy, and the exact page origin(s) that may use it.)
2. **The three room rules** (English), shown in the notice and in the server's
   `say`; and **who watches the room** (the operator console is the only tool).
3. **TTL of messages in the server's memory** (proposed 45 minutes) and how many
   messages a newcomer sees (the ring: 200). Shorter is more private.
4. **The limits**: 64 connections, 3 per address, one message per 2 s with a burst
   of 3, 120 messages a minute for the whole room, a ban of 5 minutes after 5
   mistakes in a minute. Everyone behind one NAT shares an address (and a ban).
5. **Names**: the demo picks `guest-NNNN`; `/nick` is allowed (every 10 s). Keep, or
   let the first name be chosen before connecting?
6. **When may the chat appear on the public Pages demo?** It stays hidden until a
   page is opened with `?chat=wss://...`; a visible link or a default address is
   your call, as is whether `matterm-demo` and `mattrix` should get a chat link
   (they would need a TLS WebSocket client: OpenSSL is not installed in the
   development sandbox).
7. **Five quiet seconds** before a draft is posted: keep, or lengthen? (A constant
   in `ChatClient` and, as a backstop, `RoomConfig::lockBackstop`, 6.5 s.)
8. **Starting `mattrix-chat` from inside Mattrix OS** (a host hook that launches the
   separate program) or leaving the built-in `chat` command as the only way.
9. **Wide characters and emoji** stay refused (ASCII and Latin letters only); say if
   you want more.

### Open technical points

* Resize handling in the inline renderer: the old region is left behind and a
  new one starts below (the known weak spot of inline interfaces); pulsing text
  is our own redraw, so it behaves the same in every host.
* The admin console's transport in the container: stdin through `podman attach`
  is what the files use; a Unix socket would also work.
* The web bridge and the Podman files have not been run (see section 11).

## 11. Not verified

* Run only locally and in tests (section 12): no browser, no proxy, no TLS, no Podman.
* Beast calls named above (`read_message_max`, no deflate, `got_text`) are from
  memory of its API; check against the installed version.
* The inline live-region approach, draft traffic estimates and the Podman
  hardening are untried.
* All numbers are proposals.

## 12. Running it locally

Needs Boost 1.74 or newer (header-only parts) and a C++17 compiler; `cmake -S . -B
build && cmake --build build` builds `build/chatd` and `build/mattrix-chat`
(`-DMATTERM_BUILD_CHATD=OFF` skips them). Nothing leaves the machine.

```sh
# Terminal 1: the server. The plain TCP listener is for testing and refuses any
# address that is not loopback; WebSocket needs an allowed origin.
build/chatd --origin http://localhost:8080 --listen 127.0.0.1:8097 \
            --test-tcp 127.0.0.1:8098 --console
#   operator commands in the same terminal: help, list, stats, say TEXT, kick NAME,
#   ban NAME|SOURCE [min], mute NAME [min], delete SERIAL, clear, lock, unlock

# Terminals 2 and 3: two people
build/mattrix-chat --tcp 127.0.0.1:8098 --name ada
build/mattrix-chat --tcp 127.0.0.1:8098 --name bo
#   over WebSocket instead (no TLS here; the Origin must be an allowed one):
build/mattrix-chat --ws 127.0.0.1:8097 --origin http://localhost:8080 --name cy

# Poke the protocol by hand (fields are separated by a TAB):
printf 'hello\tmallory\n' | nc 127.0.0.1 8098
```

The program first prints what it is and asks `Connect? [y/N]` (`--yes` skips the
question). Type: the others see your draft live; Enter sends at once and five quiet
seconds send too; `/nick NAME`, `/who`, `/help`, Ctrl-C leaves. For several
simulated people, a few lines of Python with `socket` and `\t` do (send
`hello\tNAME`, then `draft\t1\ttext` and `lock\t1`); `chatd_test` and
`chatterm_test` (in `test/`) show the same with real sockets and with pseudo-
terminals.

What the tests cover: two and more clients over TCP and WebSocket, hostile lines
refused and never relayed, a flood and an over-long line ending the connection, a
name taken until its owner leaves, per-source caps and a locked room, the origin
allowlist, binary and oversized frames, unsafe options refused, and `mattrix-chat`
in real pseudo-terminals (live drafts, Enter, the five-second pause, Ctrl-C, no
color, taken names, a refused connection). **Not tested:** real browsers, TLS and
a reverse proxy, many hundreds of connections, long-running soak, Podman.

---

# Appendix A: Ergo (IRC) as the fallback

If we decide against running our own server, Ergo meets the same goals with
more built in (and a larger attack surface). Its settings below were checked by
name against Ergo's own `default.yaml` and manual (`stable`, latest release seen
v2.19.1), not run. Findings worth keeping: no configurable maximum line length
(shorten on the client and set `allow-truncation: false`), and **in-RAM history is
on by default** (about a week, 2048 messages) even though persistent history is
off.

### A.1 Architecture

```
browser (Pages, https)            your host
 fakeos `chat` --> JS WebSocket --wss--> nginx/Caddy (TLS, X-Forwarded-For)
                                            |  ws (plain, loopback)
                                            v
                                        Ergo (websocket listener, 127.0.0.1:8097)
```

* **TLS is terminated in nginx or Caddy**, not in Ergo, so certificates are
  renewed in one place and the public side is `wss://chat.example.org/`. Ergo
  listens on loopback only (plain WebSocket) and honors `X-Forwarded-For` for
  peers listed in `proxy-allowed-from`, so cloaking, per-IP limits and bans see
  the visitor's address. The manual says exactly this ("Websockets through HTTP
  reverse proxies"; set `proxy: false` on that listener). A plain WebSocket
  listener without a `tls:` block is the same shape as the commented example in
  `default.yaml` minus TLS: **UNVERIFIED** until run.
* The proxy must forward the `Origin` header (Ergo does the allowlisting), the
  `Upgrade` headers and a long read timeout, and should rate-limit new
  connections too (belt and braces).
* The plain IRC ports (6667/6697) are **not exposed**; the default config's
  loopback 6667 listener is removed or kept loopback-only for operators.
* Native window and `mattrix` would need a WebSocket client in the host
  (a TLS stack we do not have today). Either the chat is web-only first, or we
  add a small client later. (an Ergo-only point; the main plan answers it in section 7).

### A.2 Server requirements and the Ergo keys that implement them

Keys are relative to `ircd.yaml`; "checked" means present in `default.yaml`.

| Requirement | Setting | Status |
|---|---|---|
| One room | `channels.default-modes: +ntC` (checked) plus operator-only creation: `channels.operator-only-creation: true` (checked); we create `#mattrix` once as an oper. Joining other channels is then impossible for guests. Whether to also add `+m`/`+r` as a standing mode is open. | checked |
| WebSocket behind TLS proxy | listener `"127.0.0.1:8097": websocket: true` (checked as the commented example); `server.proxy-allowed-from: [localhost]` (checked) | checked, plain-listener form unverified |
| Origin allowlist | `server.websockets.allowed-origins: ["https://<our-pages-address>"]` (checked; an empty list means no restriction, so it must not stay empty). Origin is a browser-enforced header, not authentication: non-browser clients can lie, so this stops other *websites* from using visitors' browsers, nothing more. | checked |
| IP cloaking on | `server.ip-cloaking.enabled: true`, `netname`, `cidr-len-ipv4/6`, `num-bits` (checked, on by default); `server.lookup-hostnames: false` (checked) | checked |
| Low flood/rate limits | `fakelag` (`enabled: true`, `burst-limit`, `messages-per-window`, `window`, `cooldown`; checked; tighten the defaults 5/2 per 1 s, e.g. burst 3, 1 per 2 s) and per-IP connection throttling `server.ip-limits.throttle/window/max-connections-per-window` (checked) | checked; numbers are proposals |
| Connections per IP | `server.ip-limits.count: true`, `max-concurrent-connections` (checked; default 16, proposal 2-3). Behind a proxy the limit applies to the forwarded client address; the proxy's own address must be in `ip-limits.exempted` (the manual says so). | checked |
| Short lines | Ergo has **no configurable maximum line length** in `default.yaml` (the protocol limit applies). We shorten messages on our side: the ChatClient refuses to send more than ~200 characters; server-side we set `server.compatibility.allow-truncation: false` (checked) so over-long messages are rejected rather than clipped, and cap `limits.nicklen`, `limits.topiclen`, `limits.kicklen`, `limits.awaylen` (all checked). | partly client-side |
| Guests without accounts | `accounts.require-sasl.enabled: false` (checked, the default). Account registration (`accounts.registration.enabled`) can be switched off so nobody can squat nicks, or left on for people who want a reserved nick: a choice only if Ergo is used. | checked |
| Nick rules | `limits.nicklen` (checked, lower to ~16), `server.casemapping: "ascii"` (checked) which restricts identifiers to ASCII; `accounts.nick-reservation` (checked) only matters if accounts are on. A "guest-N" naming scheme is easiest to enforce in the client (it picks the nick; the server still rejects bad ones). Forced renames or a nick denylist: **UNVERIFIED** whether Ergo offers a config denylist; moderators can use `/SANICK` (not checked) or UBAN. | partly unverified |
| Moderation | `oper-classes` and `opers` (checked): at least one `server-admin` and a `chat-moderator` class; passwords are bcrypt hashes from `ergo genpasswd` and **never in the repo** (never commit hashes). Tools per the manual's Moderation chapter: `/UBAN ADD|INFO` (account and IP/network bans, needs `ban`), `/DEFCON` (emergency limits, needs `defcon`), `/SAMODE`, `/SAJOIN`, channel `+m` (moderated), `+b` bans and kicks by channel ops, `ChanServ HOWTOBAN`. Capability names in `default.yaml`: rehash, accreg, chanreg, history, defcon, massmessage, metadata (`ban`, `snomasks` are named in the manual). | checked |
| No persistent history | `history.persistent.enabled: false` (checked, the default). **Note:** `history.enabled: true` is also the default, which keeps messages **in RAM** (`channel-length: 2048`, expiring after `restrictions.expire-time: 1w`) and serves them to clients via CHATHISTORY. Decide whether to disable it (`history.enabled: false`) or keep a short window: a choice only if Ergo is used. | checked |
| No secrets in the repo | never commit hashes or keys | n/a |
| TLS | handled by the proxy; `sts` is irrelevant without a public plaintext listener (manual) | checked |

### Config sketch (illustration, not a deployable file)

```yaml
network: { name: Mattrix }
server:
    name: chat.mattrix.invalid          # placeholder
    listeners:
        "127.0.0.1:8097":
            websocket: true             # plain; TLS ends in nginx/Caddy
    websockets:
        allowed-origins:
            - "https://<pages-address>" # placeholder, must not be empty
    proxy-allowed-from: [localhost]
    ip-limits:
        count: true
        max-concurrent-connections: 3
        throttle: true
        window: 10m
        max-connections-per-window: 8
        exempted: [localhost]
    ip-cloaking: { enabled: true, netname: "mattrix" }
    lookup-hostnames: false
    compatibility: { allow-truncation: false }
limits: { nicklen: 16 }
fakelag: { enabled: true, window: 2s, burst-limit: 3, messages-per-window: 1, cooldown: 4s }
channels:
    operator-only-creation: true
history:
    persistent: { enabled: false }      # RAM history: a choice only if Ergo is used
```

This is a sketch of *keys and shapes*, assembled from `default.yaml`. It has
not been parsed by Ergo (`ergo run --conf` or a `rehash` would be the real
check, on a machine that may run it).

### A.3 Operations and moderation

1. Oper up: `/OPER <name> <password>`.
2. Abuse from a nick: `/UBAN INFO <nick>`; with an account `/UBAN ADD <account>`;
   otherwise `/UBAN ADD <ip|network>` (cloaking hides the IP from users, not
   from opers). Channel level: `/MODE #mattrix +b <mask>`, `/KICK`, `+m` to mute
   the room while someone cleans up.
3. Flood of connections: `/DEFCON 4`, raise toward 2 only in an emergency (all
   new connections then need SASL, i.e. an account; guests are locked out).
4. Hosts: keep the proxy's access log short-lived; IPs of visitors are personal
   data. Say so in the notice shown by `chat`.

## Quick local test (script)

`tools/chat-local.sh` starts chatd (WebSocket plus the loopback-only TCP test port) and,
if `build-web/site` exists, a static web server for the demo, then prints the
`mattrix-chat --tcp ...` commands and the browser URL. Stop it with Ctrl+C, or from
another terminal with `tools/chat-local-stop.sh` (pid file under `$XDG_RUNTIME_DIR` or
`/tmp`, no `pkill -f`). Ports: `CHAT_WS_PORT` (8097), `CHAT_TCP_PORT` (8098),
`CHAT_WEB_PORT` (8080).

