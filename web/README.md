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

```
build/server/src/server/matterm-server --credentials ~/.config/matterm/credentials \
    --origin http://localhost:8080 --static-dir build-web/site
```

Open http://localhost:8080/, log in, and the shell appears. `login.js` posts the
password to `/login` (the session cookie is HttpOnly) and then starts
`index.js`, which opens `ws(s)://<host>/ws` itself. Without the server, open the
files from any static HTTP server with `?ws=ws://host:port/ws` to connect, or
add `?demo` to run the interactive in-browser simulator (docs/SIMULATOR.md)
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

## HiDPI

The canvas backing store is sized in device pixels (`devicePixelRatio`) and the
font is scaled by the same ratio, so text keeps its CSS size and is sharp on
scaled screens. Mouse positions map correctly (checked at ratio 1 and 2). Changing
the browser zoom after load needs a reload for the font size to follow.

## Known gaps

Copied text keeps trailing spaces of the selected line (the same model code as
the native terminal). Touch input, the `open URL` roadmap item,
and a font choice other than the bundled one are not done.
