# Chat in Mattrix OS (draft for discussion, nothing is built)

Status: **plan only.** No server config, compose file or code exists yet, by
decision (build nothing until the plan is agreed). This page collects the
options, the security requirements and the open questions. Ergo keys quoted below
were checked against Ergo's own `default.yaml` and manual (the `stable` branch;
the latest release seen was v2.19.1) fetched during drafting; anything not
confirmed there is marked **UNVERIFIED**. Nothing was run: no Ergo binary or
container was started.

## 1. What we want

A single public room, `#mattrix`, reachable from the `chat` command inside the
pretend OS (web demo, native window, `mattrix`), with guests welcome and with
moderation we can actually operate. It is off unless the visitor asks for it, and
it never reaches a server without saying so first.

## 2. Options

| Option | For | Against |
|---|---|---|
| **A. Ergo (IRC, Go, built-in WebSocket)** (the current proposal) | One static binary, TLS and WebSocket built in, moderation tools (`UBAN`, `DEFCON`, opers), IP cloaking, flood control, in-memory history only by default. IRC is a simple line protocol: easy to parse in a small C++ client. | A server to run and patch; IRC is old and loose about text, so the client must treat everything as hostile. |
| B. Our own tiny relay (one room, JSON over WebSocket) | Exactly what we need, nothing else; can live next to `matterm-server`. | We own every abuse-prevention feature (flood, bans, cloaking, moderation UI); a new network service to get wrong. |
| C. Other IRC daemons (InspIRCd, ngIRCd, ...) | Mature. | Less built in for WebSocket and moderation; more config. |
| D. Matrix/XMPP | Federated, rich. | Far too heavy for a demo; large client. |
| E. No live chat (a guestbook or links to an existing channel) | Zero operational risk. | Not what was asked. |

Recommendation: A, as asked. The cheap fallback, if running a server is not
wanted after all, is E.

## 3. Architecture (option A)

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
  add a small client later. Open question 6.

## 4. Server requirements and the Ergo keys that implement them

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
| Guests without accounts | `accounts.require-sasl.enabled: false` (checked, the default). Account registration (`accounts.registration.enabled`) can be switched off so nobody can squat nicks, or left on for people who want a reserved nick: open question 3. | checked |
| Nick rules | `limits.nicklen` (checked, lower to ~16), `server.casemapping: "ascii"` (checked) which restricts identifiers to ASCII; `accounts.nick-reservation` (checked) only matters if accounts are on. A "guest-N" naming scheme is easiest to enforce in the client (it picks the nick; the server still rejects bad ones). Forced renames or a nick denylist: **UNVERIFIED** whether Ergo offers a config denylist; moderators can use `/SANICK` (not checked) or UBAN. | partly unverified |
| Moderation | `oper-classes` and `opers` (checked): at least one `server-admin` and a `chat-moderator` class; passwords are bcrypt hashes from `ergo genpasswd` and **never in the repo** (section 6). Tools per the manual's Moderation chapter: `/UBAN ADD|INFO` (account and IP/network bans, needs `ban`), `/DEFCON` (emergency limits, needs `defcon`), `/SAMODE`, `/SAJOIN`, channel `+m` (moderated), `+b` bans and kicks by channel ops, `ChanServ HOWTOBAN`. Capability names in `default.yaml`: rehash, accreg, chanreg, history, defcon, massmessage, metadata (`ban`, `snomasks` are named in the manual). | checked |
| No persistent history | `history.persistent.enabled: false` (checked, the default). **Note:** `history.enabled: true` is also the default, which keeps messages **in RAM** (`channel-length: 2048`, expiring after `restrictions.expire-time: 1w`) and serves them to clients via CHATHISTORY. Decide whether to disable it (`history.enabled: false`) or keep a short window: open question 4. | checked |
| No secrets in the repo | see section 6 | n/a |
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
    persistent: { enabled: false }      # RAM history: see open question 4
```

This is a sketch of *keys and shapes*, assembled from `default.yaml`. It has
not been parsed by Ergo (`ergo run --conf` or a `rehash` would be the real
check, on a machine that may run it).

## 5. The `chat` command (step 2, to be built later)

**Behavior**
* `chat` alone prints what it is and does: "This connects your browser to
  chat.example.org (an IRC server run by <owner>) over wss. The server sees your
  IP address; other users see a cloaked name, not your IP. Messages in
  #mattrix are public. Type `chat on` to connect, `chat off` to leave." **Off by
  default**, nothing is contacted before `chat on` (also not by `?cmd=chat`
  unless the host passes an explicit consent flag; open question 7).
* A full-screen program (like `less`) with a message area, an input line and a
  user list; `/me`, `/nick`, `/quit`, `/help`; Esc or Ctrl-C leaves and
  disconnects. Nick: `guest-NNNN`, changeable, validated locally.
* The room rules (3 lines) are shown on join; the server's `motd` repeats them.

**Design**
* `ChatClient`: a pure C++ state machine, like `ScriptedLink::Program`: bytes in
  (decoded WebSocket text messages = IRC lines), actions out (lines to send,
  events to show), time injected. States: Off, Connecting, Registering (CAP
  LS/REQ minimal, NICK/USER, no SASL), Joined, Closing/Error. Handles PING/PONG,
  nick collision (append digits), `ERROR`, reconnect only on user request.
* An **abstract channel** (`send(line)`, `onLine`, `onClose`, `close()`), which
  ScriptedLink-style test doubles implement; the web host bridges a JS
  `WebSocket` (`window.mattermChat...`, same pattern as the beeps); no I/O in
  the fakeos core, the include check keeps holding.
* **Sanitizing everything that arrives**, before it reaches the terminal or the
  screen buffer. Server text, nicks, topics, user-list names, ERROR reasons and
  CTCP are all attacker-controlled. Rules:
  1. Decode strict UTF-8; invalid sequences become U+FFFD.
  2. Remove C0 controls (including ESC, BEL, BS, CR, LF, TAB, NUL), DEL and C1
     controls (U+0080-U+009F). Nothing from a message may form a CSI/OSC/DCS
     sequence, because the ESC (and the 8-bit C1 introducers) are gone; the
     leftover `[31m` is plain, visible text.
  3. Strip IRC formatting bytes (0x02 bold, 0x03 color + digits, 0x04 hex color,
     0x0F, 0x11, 0x16, 0x1D, 0x1E, 0x1F) rather than interpreting them.
  4. Remove bidirectional overrides/isolates (U+202A-U+202E, U+2066-U+2069),
     zero-width and line/paragraph separators (U+200B-U+200F, U+2028, U+2029,
     U+FEFF); cap combining marks per base character.
  5. Cap line, nick and message counts; truncate, never grow.
  6. The program draws through `AsciiCanvas::text` (cells, not escape strings)
     which is the second line of defence: it never emits message bytes raw.
     Chat text must never go through the shell's raw `io.out` path.
* **Tests:** table tests for each rule (ESC `[`, OSC 52 / OSC 8 / title, DCS,
  CSI with C1 introducer 0x9B, BEL, CR overwrite, bidi, formatting codes,
  overlong/invalid UTF-8), a **fuzz test** (random bytes and mutated real IRC
  lines, many seeds) asserting the invariant "the sanitized string contains no
  control and no format characters and is valid UTF-8", plus an end-to-end test
  that feeds hostile lines through `ChatClient` into a `Pc`/`TerminalModel` and
  checks that the model never changed title, cursor mode, clipboard or
  hyperlink state. State-machine tests with the scripted channel: registration,
  PING, collisions, flood on our side (client-side send limit), server `ERROR`.

## 6. Repository and secrets

* In the repo: the config **template** with placeholders, the compose file, the
  proxy example, and this page. Never: operator password hashes, the real domain
  if it is meant to stay private, TLS keys, `ircd.db`, logs.
* Operator hashes go in a local `ircd.local.yaml` (git-ignored) or a mounted
  secret/environment-rendered file; the template says where. Rotating them is a
  `rehash`.
* The container runs unprivileged and read-only with a single volume for
  Ergo's datastore (`datastore.path`, checked); no history volume if persistence
  stays off.

## 7. Operations and moderation (what a moderator needs on one page)

1. Oper up: `/OPER <name> <password>`.
2. Abuse from a nick: `/UBAN INFO <nick>`; with an account `/UBAN ADD <account>`;
   otherwise `/UBAN ADD <ip|network>` (cloaking hides the IP from users, not
   from opers). Channel level: `/MODE #mattrix +b <mask>`, `/KICK`, `+m` to mute
   the room while someone cleans up.
3. Flood of connections: `/DEFCON 4`, raise toward 2 only in an emergency (all
   new connections then need SASL, i.e. an account; guests are locked out).
4. Hosts: keep the proxy's access log short-lived; IPs of visitors are personal
   data. Say so in the notice shown by `chat`.

## 8. Open questions

1. Who runs it, on which host and domain, and who are the moderators?
2. One fixed room only, or may moderators open others?
3. Accounts: guests only, or optional registration to reserve a nick?
4. History: none (`history.enabled: false`), a short in-memory window (current
   Ergo default is 1 week, 2048 messages), or persistent (needs an explicit
   privacy decision)?
5. Rules, language and tone of the room; what is a ban offence?
6. Native window and `mattrix`: web-only first, or add a WebSocket+TLS client?
7. `?cmd=chat` and the autorun: never auto-connect (proposed), or a consent
   click first?
8. Rate-limit numbers: proposals above are guesses, to be tuned on a testnet.
9. Do we want the owner's account name, a bot, or "powered by" in the topic?
10. Link handling: the demo's URL detection must not make chat URLs clickable
    without confirmation (links are a phishing surface).
11. Name and address of the allowed origin(s): GitHub Pages and any local
    development origin (`http://localhost:8080`?) need separate entries.

## 9. Not verified

* Nothing was started: no Ergo, no proxy, no WebSocket traffic.
* Keys are checked by name and meaning against `default.yaml`/the manual, not
  by loading the file in Ergo.
* Exact fakelag and limit numbers, the plain WebSocket listener behind a proxy,
  nick denylists and the capability names `ban`/`snomasks` need a real run.
