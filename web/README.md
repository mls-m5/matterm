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
without a parameter for the built-in demo text.

## Check it in a headless browser

```
cd web/tools && npm install playwright-core   # and a Chromium for it
node e2e.js http://localhost:8080/ 'the password' out.png
```

`e2e.js` logs in, types a command and saves a screenshot (plus `out-login.png`).
`shot.js` only screenshots a URL.

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

## Known gaps

Copied text keeps trailing spaces of the selected line (the same model code as
the native terminal). Touch input, HiDPI scaling, the `open URL` roadmap item,
and a font choice other than the bundled one are not done.
