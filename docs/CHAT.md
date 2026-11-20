# Chat in Mattrix OS (plan, nothing is built yet)

Status: **plan.** Decisions so far (from the owner): our own small chat server
(a `ChatRoom` core plus a thin WebSocket layer), C++ throughout, Boost.Beast for
WebSocket, an in-RAM ring buffer and nothing on disk, no accounts, moderation
from the server's own terminal first, **no WebRTC/STUN**, and the chat client
should later also run as a **separate terminal program** that can be started
from inside the client (matterm, `mattrix`, the web demo). Ergo (IRC) stays as a
fallback and its verified settings are kept in the appendix. Nothing here has
been run; what is unverified is marked **UNVERIFIED**.

## 1. Goals and non-goals

* One public room, short plain-text messages, guests with a chosen name.
* The server never stores text anywhere but a fixed-size ring in RAM; a
  restart empties the room. No logs of message content.
* Strict input checks in several independent layers (section 4), so hostile
  text cannot reach a terminal, a file or another program.
* Not: accounts, private messages, several rooms, history search, file transfer,
  federation, IRC compatibility, peer-to-peer (WebRTC would expose every
  participant's IP to every other, needs a signalling server anyway, often needs
  TURN, and cannot be moderated).

## 2. Architecture

```
 fakeos `chat` program ----+                      +-- server terminal (admin)
 mattrix-chat (any terminal)+--wss--> nginx/Caddy --ws--> chatd
 web demo (JS WebSocket) ---+   (TLS, limits)            |  ChatRoom (pure core)
                                                          |  Beast adapter (I/O only)
```

Pieces, each separately testable:

| Piece | What | I/O? |
|---|---|---|
| `ChatRoom` | The whole server logic as a state machine: connections, names, validation, rate limits, caps, ring buffer, moderator commands. Time and randomness injected. Input: events (`connected`, `frame`, `closed`, `tick`, `admin command`); output: actions (send to connection, close connection with reason). | none |
| `chatd` | Boost.Beast server: accepts, forwards bytes and time to `ChatRoom`, executes its actions. Reads the admin console from stdin or a Unix socket. Listens on loopback; TLS ends in nginx/Caddy (as before: `X-Forwarded-For` honored only from the proxy). | yes, thin |
| `ChatClient` | Client state machine: Off, Connecting, Hello, Joined, Closed/Error; sends `hello`/`say`, applies the room's events, sanitizes everything it receives, rate-limits its own sending. Time injected. | none |
| Chat channel | Abstract connection (`send(text)`, `onText`, `onClose`, `close()`); test doubles like `ScriptedLink`; real ones: JS `WebSocket` (web), Beast client (native, see 7). | per host |
| Chat screen | The TUI (section 5) as a fakeos-style `Program` drawing into `AsciiCanvas`. | none |

The core pieces live in a library with no sockets (like fakeos, with the include
check), so they run in the web build too.

Existing server code is not a drop-in: `RateLimiter` is for password attempts
(per source, lockouts) and `ReplayRing` replays terminal bytes by serial and byte
budget; the chat needs a per-connection token bucket and a ring of fixed slots.
`matterm-server` does show the Beast setup, `proxysource` (trusted `X-Forwarded-For`)
and `sourceKey` (IPv4 as is, IPv6 as /64) that `chatd` should reuse.

## 3. Protocol (draft)

WebSocket **text** frames, one JSON object per frame, UTF-8, at most **1 KiB**.
Binary frames, fragmented messages, permessage-deflate and unknown fields are
rejected (Beast: `read_message_max`, no deflate extension, check `got_text()`).

Client to server:
* `{"t":"hello","name":"ada"}`: must be the first message, within 5 s.
* `{"t":"say","text":"hello everyone"}`
* `{"t":"ping"}` (optional; the server also pings).

Server to client:
* `{"t":"welcome","name":"ada","users":["bo","cy"],"history":[{"n":"bo","s":..., "text":"..."}]}`
* `{"t":"msg","name":"bo","text":"...","ts":1234}` (`name` is set by the server)
* `{"t":"join","name":...}`, `{"t":"leave","name":...}`
* `{"t":"error","code":"name-taken|bad-name|too-long|rate|full|banned|bad-frame","text":"..."}`, usually followed by a close.
* `{"t":"notice","text":"..."}` (moderator or server messages).

A parser for this fixed shape (not a general JSON parser: a small strict one that
accepts only these keys and string/number values) keeps the attack surface small.
**Open:** whether JSON or a trivial line format (`say<TAB>text`) is simpler and
safer; a fixed line format avoids a JSON parser altogether.

## 4. Strict input, in layers

1. **Proxy:** request size, connection and request rates, timeouts.
2. **Transport (Beast):** text frames only, 1 KiB cap, no compression, no
   fragmentation, ping/pong with timeouts (slowloris), handshake deadline.
3. **Server validation (`ChatRoom`)**, rejecting rather than repairing:
   strict UTF-8; **name** `[A-Za-z0-9_-]{2,16}`, unique among current
   connections, a small reserved list (`server`, `admin`, `system`, `mod*`,
   `you`); **text** 1..200 code points, no C0/C1 controls (also not TAB, CR, LF),
   no DEL, no format/bidi/zero-width characters (U+200B-U+200F, U+202A-U+202E,
   U+2028/2029, U+2066-U+2069, U+FEFF), no unassigned or private-use code
   points, no leading/trailing space, no run of more than N identical
   characters, not only spaces. Fixed schema: any other key is an error.
4. **Limits (`ChatRoom`):** token bucket per connection (for example 1 message
   per 2 s, burst 3) and per source (`sourceKey`), a hard total of connections
   (for example 64), per-source cap (2-3), a cap on connections still without
   `hello`, a cap on total bytes queued per connection (a slow reader is
   disconnected, never buffered), repeated violations disconnect.
5. **Storage:** a ring of fixed slots (for example 200 slots of 256 bytes) in one
   preallocated array; nothing grows; oldest dropped. **No file writes, no
   message text in logs** (counters only); the container is read-only with no
   volume and no outbound network; the process does not know how to open a file
   for chat data. Names and IPs are not stored after a connection ends (bans
   excepted, see section 6).
6. **Client (`ChatClient`):** treats everything received as hostile although the
   server checked it: decodes strict UTF-8, removes C0/C1 controls (so no
   CSI/OSC/DCS can form; the leftover `[31m` is plain text), IRC-style format
   bytes (harmless but stripped), bidi and zero-width characters, caps lengths
   and counts. Same code path as the server's validator where possible (one
   shared `chatText` function, tested once).
7. **Screen:** the chat program draws with `AsciiCanvas::text` (cells), never
   raw strings into the output stream, and never through the shell's `io.out`.
8. **Links:** URLs in chat are shown as text; the demo's link detection must not
   make them clickable without a confirmation (phishing surface).

The shared validator is the single point to fuzz hard.

## 5. The chat TUI (planning)

Sketch at 80x24 (cells; colors in brackets):

```
 Mattrix chat   #mattrix   12 here   [connected]            Esc menu   <- title bar (reverse)
+--------------------------------------------------------+------------+
| 14:02 bo   hi everyone                                 | ada (you)  |
| 14:02 cy   hej!                                        | bo         |
| 14:03 *    dee joined                                  | cy         |
| 14:05 ada  anyone seen the crumble game?               | dee        |
|                                                        |            |   <- user list (hidden < 60 cols)
|                                                        |            |
+--------------------------------------------------------+------------+
| > type here, Enter sends                              118/200   [ok]  <- input + counter + rate state
```

* **Layout:** title bar, message pane, user list (hidden below about 60 columns
  and replaced by the count in the title), one input line with a character
  counter and a small state field (`ok`, `slow down`, `muted`, `offline`).
  Works from 40x10 up; below that a one-line "terminal too small".
* **Messages:** time, name, text; names get stable colors from a hash (16/256/
  truecolor degraded like the rest of fakeos); server notices dimmed; own
  messages marked; hard wrap with a hanging indent; wide characters counted by
  cell width (single-width only at first: anything else is replaced).
* **Input:** one line editor reusing the shell's (Backspace, Ctrl-U/W/A/E, left/
  right, history of own messages with Up/Down), 200 code points max shown as a
  counter, paste is sanitized and truncated, Enter sends, an empty line does
  nothing.
* **Scrolling:** the pane follows new messages; PageUp/PageDown or the wheel
  scrolls and shows "N new below"; End jumps back. Ring-sized buffer on the
  client too (no unbounded growth).
* **Commands:** `/help`, `/nick NAME`, `/me TEXT` (action, if the protocol gets
  one), `/clear`, `/who`, `/quit`; everything else starting with `/` is an error
  line, not sent.
* **Keys:** Esc opens a tiny menu (Resume, Leave); Ctrl-C leaves at once. Mouse:
  wheel scrolls; click a name in the list inserts `name: `; no link clicking.
* **States and honesty:** connecting spinner with the server name, "reconnect?"
  only on request, the notice from section 6 shown before the first connect
  (what it is, that the server sees your IP, that messages are public and
  nothing is kept, rules in 3 lines) and again in `/help`.
* **Accessibility:** no reliance on color alone (the own-message marker and
  notices have symbols too); optional bell on mention is off by default; works
  with a screen reader mode that just prints lines without the full-screen
  layout (the standalone program's `--plain`).
* **Testing:** the screen is a `Program`, so the existing `Pc` harness checks
  rendering at several sizes, resize, scrolling, narrow terminal, 16/256/
  truecolor and hostile text, with the scripted channel driving it.

## 6. Notice, privacy, operations

* `chat` and `mattrix-chat` print, before any connection: the server's name,
  that the connection is a WebSocket over TLS, **that the server sees your IP
  address**, that other users see only your chosen name, that messages are
  public, kept only in server memory and gone at restart, the room rules. Off by
  default; `?cmd=chat` does not auto-connect without a consent step (open
  question 4).
* The server does not record IPs beyond the live connection and active bans
  (in RAM). The proxy's access log is the remaining place visitor IPs live: keep
  it short-lived and say so.
* **Moderation (first version):** an admin console on the server's own terminal
  (stdin, or a Unix socket with 0600 permissions; no accounts, whoever can
  reach the host is admin): `list` (name, age, source in this console only),
  `kick NAME`, `ban IP|NAME [minutes]`, `unban`, `mute NAME [minutes]`,
  `delete N` (remove a message from the ring and tell clients to hide it),
  `clear`, `lock` / `unlock` (no new joins), `say TEXT` (a notice), `stats`,
  `quit`. Bans and mutes live in RAM and expire; a restart clears them
  (optionally a ban file later, a deliberate decision).
* Secrets: none in the repo. The only configuration is the listen address, the
  allowed origin(s) and numeric limits; the template lives in the repo with
  placeholders.

## 7. Running the chat as a separate terminal program

Wanted later: `mattrix-chat`, started from a normal terminal, from matterm or
from inside Mattrix OS, and `chat` in the pretend OS becomes a convenience for
the same program.

* **Shared code:** `ChatClient`, the validator and the chat screen are the
  same library in every host. The screen is a fakeos `Program`; a small
  **program host** (the part of `app/demo/ttyhost.cpp` that does raw mode,
  `poll`, `SIGWINCH`, capability detection and restore) is split out so that it
  can run a `Program` directly instead of a whole `System`, and so that its
  poll loop can also wait on the socket.
* **Connection in the native program:** Beast client with TLS. This needs
  OpenSSL (`libssl-dev`), which is **not installed in this sandbox** (headers
  absent): an apt request is needed before building it. Options for the
  connection: `wss://` directly (needs TLS in the program), or `ws://` to a
  local `chatd` / an `ssh -L` tunnel for development.
* **From inside Mattrix OS (`mattrix` and `matterm-demo --tty`):** `chat` must
  not open sockets from the fakeos core (it has no I/O). Either the host
  provides the channel (web: JS `WebSocket`; native hosts: the same Beast
  client), or `chat` runs the separate program by asking the host to start it.
  The second route keeps the pretend OS pure but needs a "launch external
  program" hook in the host and a clear consent step. **Open question 6.**
* **From matterm:** `matterm -- mattrix-chat` works the same as `mattrix`:
  matterm's PTY sets `TERM`/`COLORTERM`, and capability detection already
  handles that.
* **Install:** a CMake option like `MATTERM_BUILD_MATTRIX` (the chat client
  would be `MATTERM_BUILD_CHAT`, off by default until TLS support is decided);
  the server `chatd` a separate option, Linux only.
* **Options (draft):** `--server URL`, `--name NAME`, `--plain`, `--no-color`,
  `--help`; no config file with secrets, no history file.

## 8. Build order (when told to start)

1. **Validator** (`chatText`, `chatName`): table tests, fuzz, shared by server
   and client. Smallest, most security-critical piece first.
2. **`ChatRoom` core** with scripted tests: join/leave, names, caps, token
   buckets with an injected clock, ring eviction, kick/ban/mute, slow readers,
   malformed frames, fuzz of the frame parser.
3. **Protocol parser/serializer** with strict fuzz.
4. **`ChatClient`** with the scripted channel; end-to-end with `ChatRoom` in
   process (no sockets): two clients talking, hostile messages never reaching
   the screen model.
5. **Chat screen** as a `Program` with `Pc` tests at several sizes.
6. **`chatd`** (Beast adapter, admin console); loopback integration test with a
   real socket.
7. Web bridge and `chat` command in fakeos (off by default, consent text).
8. Separate `mattrix-chat` (program host split, TLS decision).
9. Deployment notes (proxy snippet, Podman, limits), only after the above.

## 9. Open questions

1. Protocol: JSON with a strict mini-parser, or a fixed line format?
2. Numbers: ring size, message length (200?), token bucket, total/per-IP caps.
3. Names: fixed `guest-NNNN` assigned by the server, or free choice within the
   rules, and may a name be changed during a session?
4. `?cmd=chat` and autostart: never auto-connect, or a consent key press?
5. Moderation beyond the server terminal (a moderator inside the room with a
   secret key) later, or never?
6. Starting the separate program from inside Mattrix OS: host-provided channel,
   or a host hook that launches `mattrix-chat`?
7. TLS in the native client: install OpenSSL (apt request) or web-only plus
   loopback `ws://` for the native client at first.
8. Wide characters, emoji and combining marks: single-width ASCII-plus-Latin
   only at first, or full Unicode with a width table?
9. Where it runs, the domain, who watches the room, and the room rules.

## 10. Not verified

* Nothing was built or run.
* The Boost.Beast calls named above (`read_message_max`, no deflate, `got_text`)
  are from memory of its API: confirm against the installed version.
* OpenSSL headers are absent here; the native TLS client is untried.
* All numbers are proposals.

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

