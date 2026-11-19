# matterm in the browser

The terminal model, VT parser and SDL renderer compiled to WebAssembly with
Emscripten, talking the matterm-tty protocol (docs/PROTOCOL.md) over a
WebSocket to `matterm-server` (docs/SERVER.md).

## Build

Install the Emscripten SDK from its official download (the distribution
packages are too old):

```
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh
```

Then, from the repository root (SDL2 and SDL2_ttf come from Emscripten ports on
first use):

```
emcmake cmake -S web -B build-web -DMATTERM_FONT=build/assets/fonts/JetBrainsMono-Regular.ttf
cmake --build build-web
```

`build-web/site/` is the whole client: `index.html`, `index.js/.wasm/.data`,
`login.js`, `style.css`. There is no inline script or style, because the
server's Content-Security-Policy forbids it.

## Run

First create the password file with `matterm-server --setup-credentials`
(or `tools/set-password.sh`; see docs/SERVER.md, "Credentials"). Without
`--static-dir` the server finds `build-web/site` by itself.

```
build/server/src/server/matterm-server --credentials ~/.config/matterm/credentials \
    --origin http://localhost:8080 --static-dir build-web/site
```

Open http://localhost:8080/, log in, and the shell appears. `login.js` posts the
password to `/login` (the session cookie is HttpOnly) and then starts
`index.js`, which opens `ws(s)://<host>/ws` itself. Without the server, open the
files from any static HTTP server with `?ws=ws://host:port/ws` to connect, or
add `?demo` to run the pretend system (docs/FAKEOS.md: a small Unix with an intro
animation, a shell and programs to find; `?demo&nointro` skips the intro)
with no server at all. Without a server, the login form is skipped by `?demo`
only.

## Check it in a headless browser

```
cd web/tools && npm install playwright-core   # and a Chromium for it
node e2e.js http://localhost:8080/ 'the password' out.png
```

`e2e.js` logs in, types a command and saves a screenshot (plus `out-login.png`).
`shot.js` only screenshots a URL. `tools/run-e2e.sh SERVER SITE [OUTDIR]` starts
a server with a throwaway password and runs `e2e-input.js`, `e2e-copy.js` and
`e2e-lifecycle.js` against it.

## Session lifecycle

When the program exits, the connection fails or the server closes it, the
terminal prints the reason and `[Press Enter to log in again]`; Enter reloads
the page and shows the login form. A new login is needed because each login
allows a single WebSocket upgrade.

## Input

Keyboard (text, control keys, navigation, function keys, application keypad),
mouse reporting (all xterm modes and encodings, wheel), focus reporting, local
text selection (drag, Shift to select while an application reports the mouse),
Ctrl+Shift+C to copy and Ctrl+Shift+V to paste (through the page's `paste`
event, so bracketed paste works), OSC 4/104 palette changes, the title (OSC
0/2) and terminal replies (device attributes, cursor position) are wired up,
and an unfocused canvas shows the hollow cursor.

Checked with `tools/e2e-input.js` and `tools/e2e-copy.js`. Headless Chromium
cannot produce a real Ctrl+Shift+V paste, so that test dispatches the `paste`
event instead.

## The framed window

On a wide desktop viewport (`min-width: 800px` and `pointer: fine`) the terminal
is a panel on the page: narrower than the viewport (at most 1100 px), with
margins on all sides, rounded corners, a layered shadow and a hairline border.
The panel clips the canvas, so the scrollbar strip and the selection follow the
corners. The page color around it follows `prefers-color-scheme`; the terminal
palette does not change. Phones, tablets and small viewports stay edge to edge
(a media query, no user-agent checks), and the key bar and soft keyboard are
unchanged there.

A small button in the top right corner of the panel switches to edge to edge and
back. It is for the mouse only: it is invisible (and does not take clicks) until
the pointer is near that corner, or it has keyboard focus, so it never covers
terminal content, and it hands the keyboard focus back to the terminal. The
choice is stored in `localStorage` (`matterm.layout`); `?fullscreen` starts edge
to edge.

The grid comes from the panel: the canvas fills it, and a `ResizeObserver` in
`login.js` sets the backing store to the canvas's exact device pixels
(`devicePixelContentBoxSize`, falling back to `clientWidth * devicePixelRatio`),
so window resizes, rotation and the toggle all follow the same path (`main.cpp`
sees the new size, resizes the model and tells the server).

"Embedded" here means the framed window. Embedding the page in another page with
an `<iframe>` stays blocked by the server's headers (`frame-ancestors 'none'`).
There is no title bar inside the frame: it would cost a terminal row of height,
and the program's title already appears in the browser tab (`document.title`).

## Phones and tablets

On touch devices (`pointer: coarse`, or `?touch` to force it) tapping the
terminal focuses a hidden field, which opens the on-screen keyboard; its text
goes to the terminal. A bar adds the keys phone keyboards lack: Esc, Tab, Ctrl and
Alt (tap to latch for the next key), arrows and `| / - ~`. The page resizes when
the keyboard opens (`interactive-widget=resizes-content`). Hardware keyboards
work as on the desktop. Checked with touch emulation in `tools/e2e-touch.js`
(a real Android keyboard has not been tried: its composition and backspace
behavior varies by keyboard app).

### Touch gestures

Handled in `main.cpp` with the classifier in `src/frontend/touchgesture.h`
(SDL finger events; SDL's own touch-to-mouse emulation is off).

| Gesture | Primary screen | Alternate screen | Application mouse reporting |
|---|---|---|---|
| Tap | click: clears the selection, keyboard opens (`login.js`) | same | press and release of the left button |
| One-finger drag | scrolls the scrollback (smooth, no inertia) | arrow keys, one per text row | wheel presses at the finger, one per row |
| Long press (about 0.45 s), then drag | selects a word, dragging extends; lifting copies it to the clipboard | same | not available: the application owns the mouse |
| Two-finger pinch | font size in 1 px steps (8 to 40 CSS px), stored in `localStorage` as `matterm.fontSize` | same | same |

Ctrl+wheel changes the font size on the desktop. A drag or pinch never focuses
the keyboard field or toggles the keyboard; only a short single-finger tap does.
There is no Shift on a phone, so local selection while an application has the
mouse is not possible by touch. Changing the font rebuilds the renderer, then the
grid is re-fitted and the server told through the normal resize path. Touch events
wake the idle-throttled loop, which stays awake while a finger is down.
`tools/e2e-gestures.js` checks these with CDP touch events.

## Demo page parameters

For the static demo (`demo.html`, or `index.html?demo`; see docs/DEMO.md):

| Parameter | Effect |
|---|---|
| `?embed` | Edge to edge for a page inside an `<iframe>`; no layout toggle. |
| `?nointro` | Skip the intro animation. |
| `?noreveal` | Print program output at once, not at a pace. |
| `?cmd=...` | Type and run a command at the first prompt (text only, 200 characters). |
| `?theme=light\|dark` | Page colors around the terminal (not the terminal palette). |
| `?smooth=N` | Cursor glide in milliseconds; on (80) in the demo, `?smooth=0` turns it off. |

While the page is out of view (a hidden tab, an iframe scrolled away) the loop
stops drawing (`window.mattermPaused`, set from `visibilitychange` and an
`IntersectionObserver`).

## HiDPI

The canvas backing store is sized in device pixels (`devicePixelRatio`) and the
font is scaled by the same ratio, so text keeps its CSS size and is sharp on
scaled screens. Mouse positions map correctly (checked at ratio 1 and 2). Changing
the browser zoom after load needs a reload for the font size to follow.

## Known gaps

Copied text keeps trailing spaces of the selected line (the same model code as
the native terminal). The `open URL` roadmap item
and a font choice other than the bundled one are not done.
