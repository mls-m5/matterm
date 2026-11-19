# Task: open links with Ctrl+click

Assignment for an independent agent in its own worktree (`.tmp/agent3`, new
branch `agent3-url` from `main`). Read first: Agents.md, docs/PARALLEL_WORK.md,
docs/SELECTION.md and `src/frontend/textselection.h` (word boundaries across soft
wraps already include URL characters), the ROADMAP.md item "URL detection and
opening", and the mouse code in `app/ptyfrontend.cpp` and `web/main.cpp`.

## Decision (from the owner)

Ctrl+left click opens a link. A plain click keeps its meaning (selection, or
application mouse reporting).

## Scope

1. **Detection** as pure CPU-only logic next to `textselection.h`: given the model
   and a cell, return the URL under it (text, and the cell range for
   highlighting) or nothing. Handles URLs wrapped across rows, trims trailing
   punctuation and unmatched closing brackets (`.,;:!?)]}>'"`), accepts only
   `http://` and `https://`, caps the length (2048 bytes), rejects control
   characters, and never returns anything for an empty or malformed URL.
2. **Opening, native**: Ctrl+left click opens with `SDL_OpenURL`, after the
   detection above; report failure in the window title like other errors. Do not
   use a shell or `system()`.
3. **Opening, browser**: Ctrl+left click calls `window.open(url, "_blank",
   "noopener,noreferrer")` through `EM_ASM` (a user gesture is required, so call
   it in the click handling path). Pass the URL as a string argument, never by
   building JavaScript from it.
4. **Feedback**: while Ctrl is held and the pointer is over a URL, underline it
   (a renderer-side overlay or an existing attribute path, whichever is least
   invasive; no model changes) and show the target, for example in the window
   title or document title. Clear it when Ctrl is released or the pointer leaves.
5. **Interplay**: when an application has mouse reporting on, Ctrl+click is not
   consumed for links unless Shift is also held (Shift already hands the mouse
   to local handling); document the rule.

## Out of scope

OSC 8 hyperlinks, other schemes (`file:`, `mailto:`), a confirmation dialog,
right-click menus, touch (long press), protocol and server.

## Owned and shared paths

Owned: new files under `src/frontend/` and their tests. Shared, edit minimally and
report: `app/ptyfrontend.cpp`, `web/main.cpp` (click and hover handling only; the
integrator edits other parts), and the renderer only if the underline overlay
truly needs it.

## Acceptance

- Unit tests: plain URL, URL with query and fragment, wrapped URL, trailing
  punctuation and brackets, schemes other than http/https rejected, length cap,
  no URL, cell on the boundary.
- Security tests: `javascript:`, `data:`, URLs containing quotes or `\`, and
  control characters are never returned.
- Default suite passes; the browser build compiles if `emcmake` exists (if not,
  say so; the integrator will verify with a headless browser).
- clang-format applied; no build products committed. Update the ROADMAP item and
  add a short section to docs/SELECTION.md or a new docs/LINKS.md.

## Handoff

As in docs/PARALLEL_WORK.md. Do not push.
