# Several terminals in one window, persistent sessions and recording

Status: server sessions (phase 1) and the browser tabs with two panes (phase 2
and part of 4) are implemented; recording, settings and the native client are
not. Decisions are marked **Decided** (agreed with
the owner), **Proposed** (recommended, confirm before building) or **Open**.
This replaces the "Session Multiplexing" notes in ROADMAP.md as the place to
refine the design; settings UI work comes after this (settings may be per window
or per terminal).

## 1. Goals and non-goals

Goals:

- Several independent terminals in one browser window (tabs first, split panes
  later), and later in the native app.
- Shells can survive a closed tab, a reload or a dropped network and be
  reattached, **optionally**: with the feature off nothing extra is kept in memory
  and a session ends with its connection, as today.
- The same mechanism serves a later *timed recording and playback* feature.
- One emulator pipeline: the server never parses VT output.

Non-goals (for now): a tmux replacement, server-side screen state or diffs
(mosh-style state sync), several viewers typing into one session, sharing a
session between users, native TLS.

## 2. Layers and what stays as it is

```
tty  ->  bytes  ->  [ transport ]  ->  parser  ->  model  ->  renderer
```

- **Decided:** the server relays raw PTY bytes; the client owns parser, model and
  renderer. The native app already runs `PtyLink` in-process (no transport), the
  browser runs `ClientSession` over a WebSocket. Both are `TerminalLink`s.
- A screen-update protocol (server emulates, sends cell diffs, client sends
  input events) would be a second transport implementing the same `TerminalLink`.
  Not planned; the seam exists if it is ever needed.

## 3. Sessions are owned by the server, independent of connections

Status: implemented (`src/server/sessionregistry.*`, docs/PROTOCOL.md section 11,
docs/SERVER.md "Sessions"). Before it, one login allowed one WebSocket upgrade and
the PTY lived and died with that connection (`PtySessionHost`). The change:

- **Implemented:** a `SessionRegistry` in the server owns terminal sessions
  (`sessionId`, PTY, child process group, optional replay buffer, state:
  attached or detached, creation time, last activity). A connection attaches to a
  session and detaches when it closes; closing the connection does not kill the
  session unless persistence is off.
- Session ids are random 128-bit values, only valid for the logged-in owner,
  never derived from anything guessable, never put in logs.
- **Implemented:** one WebSocket per terminal, always the bare `GET /ws`: the
  terminal is chosen in the Hello message (an empty id is a new terminal,
  otherwise the 32-hex id), never in the URL, so ids stay out of logs. The
  existing protocol is reused; each login may hold several concurrent upgrades
  (`max_upgrades_per_login`, 16). A later protocol v2 with channel ids on one
  connection stays possible and is not needed for tabs.
- Limits: maximum sessions, maximum detached sessions, `detached_timeout`
  (a detached session is killed after this long; `0` = never), and the existing
  per-connection limits. A shell that exits ends its session; the client is told.
- A session may have at most one attached connection at a time in v1. Attaching
  a second one either takes over (the old one is closed with a reason) or is
  refused. **Decided:** take over; the old connection is closed with a reason, so
  moving a terminal to another window just works.

## 4. Replay buffer (resume without a server-side parser)

- **Implemented:** each session keeps a bounded ring of the most recent raw output,
  stored as chunks `(timestamp, bytes)`. On reattach the server sends the ring to
  the client, whose parser rebuilds the screen, then continues live. The client
  tells the server its size; the server resizes the PTY (which makes programs
  receive `SIGWINCH` and redraw).
- Off switch: `replay_buffer_bytes` = `0` disables the buffer and persistence
  entirely (the session ends with its connection). A per-session cap and a total
  cap bound memory; the oldest chunks are dropped first.
- Known limits, accepted at first: the buffer may start inside an escape
  sequence (align the start at a newline or drop bytes until a safe point);
  modes set before the buffer starts (alternate screen, mouse reporting,
  bracketed paste) are lost until the program sets them again; scrollback is only
  as long as the buffer. **Open:** if this bites, the server keeps a tiny scanner
  for private-mode set/reset sequences (`?1049`, `?2004`, `?1000..?1006`, `?25`)
  and prefixes the replay with the current mode state. This is a string search,
  not a parser.
- Protocol: the resume fields of docs/PROTOCOL.md section 11 (`sessionId`,
  `resumeOffset`, `Welcome.flags`), negotiated through `features` bits. The
  offset counts output bytes since session start so a client can resume from where
  it stopped when the data is still in the ring, and otherwise gets a full replay.

## 5. Client structure (browser, later native)

Status: implemented for the browser (phase 2, and two panes from phase 4). The
browser parts (WASM and page) are type-checked and unit-tested but their e2e
checks (`web/tools/e2e-tabs.js`, `e2e-sessions.js`) are run by the integrator.

- **Portable, SDL-free core** (`src/frontend/`, header-only, native tests):
  - `TerminalView` owns one terminal: `TerminalModel`, `VtParser`, palette,
    title, selection and cursor glide, fed by one `TerminalLink`. It keeps
    parsing while not shown, tells how its link ended (exited, taken over by
    another window, session unavailable, connection lost) and never reconnects
    by itself. A full replay (`Welcome.flags` reset) clears it completely.
  - `TabModel` is the data side of a tab strip: ordered tabs, the tab in each
    pane, activity marks, a compact flag and at most two panes with a clamped
    divider that collapses to the focused pane in a narrow window. No rendering
    and no terminals, so a native client can reuse it.
- **Browser:** `web/main.cpp` holds a list of views (each with its own channel and
  `ClientSession`) and renders the shown ones; every view keeps ticking,
  acknowledging and following the size of its pane. One canvas; each pane is
  drawn into a viewport (`SdlBackend::setViewport`). The frame signature covers
  all panes, so an idle window costs nothing.
- **Tab strip** (decided with the owner): HTML/CSS **above** the canvas
  (`web/tabs.js`), accessible and themable, mouse/touch only (no key bindings,
  see section 11). It can shrink to small numbered boxes (compact mode). Actions:
  new terminal, select, close, split/unsplit, compact, reconnect, take back,
  close all sessions, log out. Titles come from programs and are shown as plain,
  short text only.
- **Panes** (decided with the owner): two terminals side by side when the window
  is wide enough (each at least 24 columns), with a draggable divider; at most
  two for now. A click or the wheel in a pane moves the keyboard focus there.
- **Explicit actions only.** Nothing reconnects, attaches or takes a terminal back
  by itself. A view whose connection dropped offers Reconnect (attach again from
  the offset it stopped at); a view that was taken over offers Take back; a new
  browser window is *offered* the terminals the first one had and attaches them
  only when the user says so. Enter in an ended terminal closes its tab (the
  last one returns to the login form).
- **Closing** sends the protocol `Close`; the tab stays until the server
  confirmed (Exit, or a normal 1000 close), and a network failure keeps the tab
  and the id. **Logout and close-all** count only when the authenticated POST
  succeeded. The page remembers only validated opaque ids
  (`web/sessionids.js`): the ones this window had (sessionStorage, attached again
  after a reload) and every id the browser had (localStorage, offered only).
- **Native, later** (design only): the same `TerminalView` and `TabModel`. Tabs or
  small boxes at the top *or* at the left, together with a borderless window in
  which they are part of a thicker, own title bar (window chrome drawn by the
  application: drag area, caption buttons, the compact strip). The native app
  already runs `PtyLink` in-process, so a view there is a `TerminalView` over
  that link; moving a terminal between windows is "attach the same session"
  where the server is involved and a hand-over of the view object otherwise.
  Not started; the browser strip is the reference for behavior.
- Known limits: no tab reordering by drag, no key bindings, touch cannot drag the
  divider, only two panes, no per-pane font size.
- The framed window layout (docs/tasks/window-frame.md) is the container for the
  tab strip; the page keeps the window a whole number of cells high with the
  strip inside it.

## 6. Recording and playback (later)

- Format (**Proposed**): the replay ring's chunks, so recording and replay share one
  structure. A recording is a header (version, size, start time, optional title)
  and a sequence of events `(delta ms, kind, bytes)` with kinds `output`,
  `resize`, and optionally `input`. Text-based and easy to inspect, in the
  spirit of asciinema's `.cast` (newline-delimited JSON), so existing players and
  tools stay usable. **Open:** adopt asciicast v2 exactly, or a close variant (a variant would only be
  for things v2 cannot express, such as a recorded `focus` event or a per-chunk
  checkpoint for fast seeking; keep the header compatible so existing players
  still open the file).
- Input is **not** recorded by default (passwords typed with echo off never appear
  in output, but input would contain them). Recording is off by default, files are
  created with mode 0600, and the owner chooses where they go.
- Playback: a `TimedLink` (a `TerminalLink` that replays events at the original
  pace, with speed control, pause and seek by replaying from the start or from a
  stored checkpoint) feeding the normal parser, model and renderer. Related to the
  existing `ScriptedLink`, which the browser simulator already uses.
- Where recording happens: server side (works with any client, can record while
  detached) or client side (no server change). **Open:** start with the server
  because it already has the timed chunks.

## 7. Configuration (implemented except recording; see docs/SERVER_CONFIG.md)

| Setting | Default | Meaning |
| --- | --- | --- |
| `persistent_sessions` | `false` | keep sessions after the connection closes |
| `replay_buffer_bytes` | `0` | per-session ring size; `0` = no buffer, no persistence |
| `replay_total_bytes` | 64 MiB | cap over all sessions |
| `max_sessions` | 16 | simultaneous sessions |
| `max_detached_sessions` | 16 | sessions with nobody attached |
| `max_upgrades_per_login` | 16 | concurrent WebSocket upgrades of one login |
| `detached_timeout_seconds` | 3600 | kill detached sessions after this; `0` = never |
| `recording_dir` | empty | empty = no recording (not implemented) |

Every number is configuration, none is a protocol constant, as for the existing
limits (docs/SERVER_CONFIG.md).

## 8. Security notes

- Session ids and attach rights belong to the single owner login; attaching
  requires a valid session cookie and the existing Origin/Host checks.
- A detached session is a running shell with no one watching: make the timeout
  visible in the UI and documented; "kill all my sessions" is a first-class action
  (also on logout).
- The replay buffer and recordings contain terminal output, which can include
  secrets. They live in memory (buffer) or in 0600 files (recording), are never
  logged, and are wiped on session end.
- More connections per login change the rate-limit and `SessionStore` rules (a
  login may now hold several concurrent upgrades, `max_upgrades_per_login`); the
  security review's findings and `trusted_proxy` apply here. Re-review after this
  feature.

## 9. Phases

1. **Server sessions and resume (no UI change).** `SessionRegistry`, detach/attach,
   replay ring with the off switch, protocol resume negotiation, limits and
   timeouts, tests (dropped connection, take-over, timeout, buffer wrap, memory
   caps, shell exit while detached). Fuzz the new message fields.
2. **Client: several views and tabs** in the browser: `TerminalView` refactor of
   `App`, tab bar, background parsing, per-view throttling, e2e tests. *Done*
   (see section 5; the browser e2e runs are the integrator's).
3. **Recording and playback**: format, server-side recorder, `TimedLink` player
   and a small player UI.
4. **Panes** (splits, focus, resize). *Two side by side in the browser are done*;
   more than two, vertical splits and a layout tree are not.
5. **Settings UI** on top of the per-window/per-terminal structure.
6. **Native app** support for the same model (separate windows first, as in the
   ROADMAP item about moving sessions between windows in one process).

Phases 1 and 2 are independent enough to run in parallel once the protocol
fields are fixed; phase 2 touches `web/main.cpp` heavily, so it should not overlap
with other work in that file.

## 10. Questions that were open before phase 1

1. Exact resume fields and the `features` bit: settled, docs/PROTOCOL.md section 11
   (feature bit `0x00000001`, ids in Hello, absolute output offsets).
2. Whether non-persistent mode (`replay_buffer_bytes = 0`) can still allow several
   simultaneous terminals: yes, they end with their connection.
3. asciicast v2 or a variant for recordings (see section 6): still open.

Settled: a second attach takes over (section 3); tab key bindings wait (section 11).

## 11. Keyboard capture (unsolved, parked)

A terminal wants every key; the browser reserves some for itself. Ctrl+L, Ctrl+R,
Ctrl+K and similar can be taken with `preventDefault()` on keydown, so the
terminal gets them but the browser shortcut stops working while it has focus.
Ctrl+W, Ctrl+T, Ctrl+N and Ctrl+Tab cannot be intercepted by a page at all in most
browsers. In full screen, Chromium's Keyboard Lock API
(`navigator.keyboard.lock()`) can capture most of them, including Esc. A good
policy needs a decision on which keys the terminal gives back (for example a
prefix chord or a "release keyboard" toggle) and per-browser testing. Parked until
there is a concrete proposal; no change to the current behavior (the page captures
what it can, see `web/main.cpp`).
