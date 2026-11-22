# Handover: integrator (n27l7) and project state

Written 2026-11-22 for new agents taking over. The owner speaks Swedish (answer in Swedish) and often
works away from the keyboard; keep the other agents busy and report plainly, including what was
**not** verified.

## The project

`matterm` (repo `mls-m5/matterm`): a native terminal emulator (SDL), a browser client (emscripten),
`matterm-server` (serves the browser, owns PTYs or proxies to the daemon), `matterm-daemon` (keeps
terminals alive when the window closes), a "Mattrix OS" demo (fakeos) and a chat (`chatd`).

## Roles and process

- **Integrator** (this file): merges agent branches into `main`, runs ctest + the browser e2e, fixes
  small breakage, writes docs, talks to the owner. Agents merge `main` into their own branch first; the
  integrator merges the agent branch back (fast, after green).
- **agent2** (`ln2ow`, branch `agent2-daemon`, worktree `.tmp/agent2`): daemon, server, ssh remote,
  reflow, CPU. See `docs/handover/agent2.md`.
- **agent3** (`rd9tj`, branch `agent3-fakeos`, worktree `.tmp/agent3`): frontend (tabs, panes, palette,
  copy mode, sessions, attach), web client. See `docs/handover/agent3.md`.
- Channels: `mcp__pis-host__send_message` (one message, wait for the answer) and the file channel
  `.tmp/agentN/FROM_INTEGRATOR.md` / `TO_INTEGRATOR.md`.
- **Pushes**: only when the owner asks, only through `request_host_git` (full 40-hex SHA, one push per
  approval). The owner now pushes himself. The repo is **public**: no server/deployment instructions, no
  secrets. No licence for now.
- No solutions that need `.bashrc` edits.

Dependencies and install commands: `docs/DEPENDENCIES.md`.

## Verification recipe (what "green" means)

```
cmake --build build -j8 && cmake --build build-web -j8     # native and emscripten
ctest --test-dir build -j4                                  # ~108 tests (soak is slow)
MATTERM_DAEMON=$PWD/build/matterm-daemon NODE_PATH=$HOME/pw/node_modules \
  sh web/tools/run-e2e.sh $PWD/build/src/server/matterm-server build-web/site /tmp/e2eout
```

- Use `build/src/server/matterm-server`, not `build/server` (stale, one WebSocket per login).
- `run-e2e.sh` skips `e2e-local.js` unless `MATTERM_DAEMON` points at the daemon.
- The page frames the terminal: the tab strip is at (43, 44) in a 1000x640 viewport, not (0, 0).
- `fakeos_about_test` counts repository files at configure time: red until `cmake` is re-run after
  files or docs change. `chatd_soak` / `fakeos` can fail once under load; they pass alone.
- `EM_ASM` bodies must not contain commas outside parentheses (object literals) or `===`.
- Nobody here can run the GUI (no display), real sshd or a browser except via Playwright.

## What was built (since the early tabs work), in the order the owner asked

1. **Chat server** default `wss://xn--laserskld-67a.se/matterm`; podman/nginx deployment stays out of
   the repo (public).
2. **Selection polish**: single cell visible, Shift+click extends from the anchor, double click selects a word.
3. **Tabs, panes, splits** (cells chrome, split tree, tab = a layout, pixel splits proportional, prefix
   Ctrl+B in the browser, GNOME shortcuts on desktop, scrollbar in the divider). `docs/TABS_PLAN.md`.
4. **Command palette** (Ctrl+Shift+P / Ctrl+B ':', button at the end of the strip), forgiving fuzzy
   search showing key strings, **keyboard shortcut view** (Ctrl+Shift+H, Ctrl+B ?), which-key box.
5. **Sessions**: save/restore (named by the first tab, "Save session as", delete), close confirmation
   (end / no / keep running), tab naming.
6. **Local daemon** `matterm-daemon` (default for the native app; `--no-daemon`; falls back with a note).
   Share button (grey/red), shared terminals only are visible to a browser via
   `matterm-server --expose-local-sessions` ("Local terminals..."), take-over notice with Enter to take
   back. **Attach terminal...**, attach here, kill terminal, detach/attach whole layouts, rename terminal.
7. **Remote daemon**: `matterm --daemon-host user@host` (ssh unix-socket forward), copies a static
   `matterm-daemon` if missing (`--daemon-install`, `--yes`); `MATTERM_STATIC_DAEMON` is ON by default.
8. **Reflow**: history reflows on width change; the live screen reflows above the cursor row only
   (bash redraws its prompt; reflowing the cursor row doubled it). `docs/SCROLLING_AND_REFLOW.md`.
9. **CPU**: the native loop drew everything every iteration; now frame signatures + sleeping loop
   (`docs/CPU.md`). Owner confirmed it works.
10. **Copy mode** (Ctrl+B [, Ctrl+Shift+Space): virtual cursor, search, block select. Smooth pixel scroll
    stays default; `--snap-scroll` / `?snap=1` snaps to rows.
11. **Tab activity indicator**: static green icon for output the user did not trigger (5 s ignore after
    own input and after a pane starts); `--tab-activity=icon|pulse|off`. Shell completion
    `matterm --completion bash|zsh|fish` (zsh/fish untested).

## Owner decisions and standing wishes

- Sharing = remote code execution: private by default, explicit flag, SSH forward / LAN only.
  "Attach terminal..." in another native window is deliberately not gated by sharing but is marked.
- Reflow chosen over keep-clipped; steady-state performance matters more than resize speed.
- Sessions are not restored automatically; a command line is saved but never started.
- Unix sockets may not be the best transport; fine for now.
- Wrapped-line marker (triangle) for reflowed lines: API prepared (`RenderFrame::wrapMarks`), not drawn yet.
- Styling of borders between terminal and palette and of popups: to be discussed.
- A real config file / settings view does not exist; flags stand in (`tabActivity`, snap scroll, ...).
- Daemon option flags: `matterm-daemon`, `--expose-local-sessions`, `--allow-remote-listen`,
  `--daemon/--no-daemon`, `--daemon-socket`.

## Open work (at the time of writing)

- agent3: borderless window (`--borderless`, hit test, minimise/maximise/close in the strip, fallback to the
  system frame) and a `matterm --window-test` probe the owner runs on GNOME.
- Draw the wrapped-line marker now that reflow exists.
- Config file and settings model/view; reconnect for `--daemon-host`; maybe a native WebSocket client.
- Make the `fakeos` count test independent of build time.
- Still never run on a real display: daemon panes, share, attach/kill/detach layout. Never run against a
  real sshd: `--daemon-host`. Weekly clang-tidy workflow untested.
- Remind the owner to try these (also in the assistant memory `remind-owner-to-test`).

## Gotchas learned

- Layout restore in the browser needs a server with `persistent_sessions: true` and a replay buffer;
  sessions per login lease are capped at 16 (e2e uses a fresh server for the limit test).
- dash with `set -e` propagates a failing `kill` in an EXIT trap: guard with `|| true`.
- A cached `find_library` result keeps a shared libsodium when switching to a static link: separate cache entries.
- Do not start daemons in tests without cleanup; stray `matterm-daemon` processes were left by tests.
