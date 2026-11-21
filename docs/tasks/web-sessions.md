> **Status (2026-11-21): paused.** The operator wants to plan tabs and views further
> before continuing. The implementation (TerminalView, TabModel, tab bar, split panes,
> id storage, SdlBackend::setViewport, ClientSession::closeCode) is archived on the
> branch `archive/web-tabs` and is not on `main`. This document and
> docs/MULTI_TERMINAL.md section 5 are kept as the design record.

# Task: browser terminal views and server-session attachment (phase 2)

Assigned by the integrator to agent2 in `.tmp/agent2`, branch
`agent2-websessions`. Starting point: merged main plus the later server/client
handshake regression fixes already on the agent branch. Read Agents.md,
docs/MULTI_TERMINAL.md and docs/PROTOCOL.md section 11 before implementation.

## Goal

Use the server-owned sessions from the browser: optional attach/full replay on
reload, deliberate take-over from another browser tab, multiple terminal views
inside one window, and minimal authenticated logout/close-all controls. Preserve
the static demo and existing input, touch, copy, link and framed-layout behavior.
No keyboard shortcuts for switching tabs (the agreed plan leaves those open).

## Contract / boundaries

- Own `web/` and new portable `src/frontend/terminalview.*` plus focused tests.
  Keep the renderer/window shared; each view owns its parser, model, palette,
  transport/session, selection and activity/render state. Parse and ACK inactive
  views without rendering them; input and clipboard go only to the selected view.
- A typed, read-only ClientSession error-code accessor may be added, with tests,
  to distinguish take-over/unavailable ids from a network failure without parsing
  human text. No wire layout or server-session ownership changes.
- IDs are selectors in Hello only, never URLs. Store only validated opaque ids,
  not tokens, terminal output, parser snapshots or potentially secret titles.
  Reload reconstructs from offset zero with the reset callback. A reconnect
  within the same live model may retain its parsed offset/state.
- A replaced view must not auto-reconnect and steal the session back. UI actions
  are explicit. A closed/expired session is not silently replaced with a shell.
- Logout and close-all must await successful authenticated same-origin POSTs;
  do not pretend that a network failure killed server PTYs. Closing a view must
  not silently forget an unconfirmed persistent session.
- Keep static demo startup independent of login/session storage. Do not add
  panes, recording/playback, settings UI or new server endpoints.

## Tests first / validation

Add native fake-channel tests for TerminalView isolation, replies, background
parsing/ACKs, reset/replay and failure/exit lifecycles. Add dependency-free Node
unit tests for browser id storage and state policy where possible. Add an e2e
script under web/tools covering reload/same PID, take-over without reconnect
ping-pong, view switching/background output, close-all and logout. Run existing
browser checks as well when Emscripten/Chromium/Playwright are available.

Build with warnings-as-errors, run the native suites, and use clang-tidy on changed
native files. Report plainly any unrun WASM/browser checks, including toolchain
or container limits. Commit coherent tests before fixes/features; no push.

Shared files: native test registration, web/CMakeLists.txt, transport accessor,
ROADMAP.md, docs/MULTI_TERMINAL.md and the server/protocol usage documentation.

## Status (branch agent2-websessions)

Done: `TerminalView` and `TabModel` (src/frontend, native tests), the typed
`ClientSession::closeCode()`, `SdlBackend::setViewport`, `cellAt` for pane-aware
mouse mapping, the tab strip and views in `web/`, id storage and tab policy with
Node tests (CTest `web_tabpolicy`, `web_sessionids`), two panes with a divider,
and the e2e scripts `web/tools/e2e-tabs.js` and `e2e-sessions.js` (wired into
`run-e2e.sh` with a second, persistent server).

Not run in the agent sandbox: the WASM build and every browser check. The C++ of
`web/main.cpp` was type-checked natively against a stub `emscripten.h`.
Shared files touched: `src/render/sdlbackend.*`, `src/frontend/sdlinput.h`,
`src/transport/clientsession.*`, `test/CMakeLists.txt`, `web/CMakeLists.txt`.
