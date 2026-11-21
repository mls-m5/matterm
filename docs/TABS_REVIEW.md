# Tabs and panes: security and robustness review

Independent review of the tab code (agent2, after the tabs work merged). Method as in
`SESSIONS_REVIEW.md`: threat model, findings written as tests first, then fixes only where the
code is ours and the fix is small. **The browser side was read, not run**: this machine has no
playwright-core and no web build, so `web/tools/run-e2e.sh` (use `build/src/server/matterm-server`;
`build/server/...` is stale) was NOT run. Every web finding below comes from reading
`web/main.cpp`, `web/websocketchannel.cpp` and the server.

## Threat model

| Input | Who controls it | Trust |
|---|---|---|
| Pane titles (OSC 0/2, up to 2048 bytes) | any program in a pane, so also `cat`-ed files | hostile |
| `sessionStorage` layout string | page scripts of the same origin, a user editing storage | untrusted |
| Session ids in that string | same | untrusted (foreign ids are only usable if the login owns them) |
| Prefix and shortcut keys | the user; auto-repeat from a held key | trusted but noisy |
| Number of panes | the user | bounded by us |

## Findings

| # | Sev | Finding | Status |
|---|---|---|---|
| F1 | high | **Web: closing a pane never sends Close.** `closePane` drops the channel (WS 1000). With persistent sessions the server keeps the session detached (1 h, max 16), so closed panes leak sessions and 16 closed panes block every new tab or split ("session limit"). | Tests `TabsReview` (server: drop fills the limit, Close frees it). Web fix for agent3: send Close before dropping the channel. |
| F2 | med | **No window-wide pane cap.** 8 per tab, unlimited tabs; the login has 16 leases (`max_upgrades_per_login`) and `layoutsnapshot` accepts 16 panes. Pane 17 fails to connect, and a saved layout with more than 16 panes is rejected on restore (layout lost). | `Workspace::setMaxPanes` (new/split ignored at the limit), tested. Web should call `setMaxPanes(16)` and show a notice on refusal (the "upgrade limit" error is tested server-side, the client text is not). |
| F3 | med | **Titles were not sanitized fully.** `displayWidth` kept U+00AD, U+061C, U+180E, U+2028/9, U+FFF9..B and tag characters (U+E0000..7F); bidi marks and overrides were already dropped. | Fixed in `tabstrip.h`, test "hostile title". The native window title (`SDL_SetWindowTitle`) takes the raw title (the OS draws it, control characters are not honoured there); left as is. |
| F4 | med | **`persistent_sessions=false` (default): restore is silently impossible.** The layout is saved, reload attaches nothing, the user gets one fresh pane and no hint. | Web: say so ("layout needs persistent_sessions") or do not save the layout. Not changed here. |
| F5 | low | **Native: held shortcut leaked.** Auto-repeat of Ctrl+Shift+E skipped the layout code and fell through to a control-character send. | Fixed: `WorkspaceView::key(.., repeat)` swallows repeats of shortcuts and forwards other held keys without starting the prefix. Tests. |
| F6 | low | **Restore storm.** One restore opens up to 16 sockets at once. Server-side this is fine (tested: 16 stale sockets are transferred, not refused), the login lease limit is the real bound. | Holds. |
| F7 | info | All restored sessions dead (server restarted): the page should fall back to one fresh pane; not verified in a browser. | Open, agent3. |

## What holds (tests)

- `layoutsnapshot.h` is strict: version, counts, ranges, ids; garbage and over-limit input are rejected whole.
- The server never lets a login attach a session id it does not own (SessionsReview), so a forged id in storage only fails that pane.
- Restore with 16 old sockets still open succeeds through transfer; at the lease limit the error is clean and recovers.
- Prefix keys: 2 s timeout, unknown second key forwards both keys; Shift is taken from the key press (merged on main).

## Tests added

| File | Covers |
|---|---|
| `test/tabsreview_test.cpp` | F1 and F6 server side, lease limit and recovery |
| `test/tabstrip_test.cpp` | F3 hostile titles |
| `test/workspaceview_test.cpp` | F5 repeat, F2 `setMaxPanes` |

## Proposed for agent3 (web, not changed here)

1. Send Close on pane close (F1). 2. `setMaxPanes(16)` and a visible message when refused (F2).
3. Warn when the server is not persistent, handle all-dead restore (F4, F7).
