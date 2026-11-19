# The matterm demo

A real terminal with a fake operating system: matterm's own terminal emulator and
renderer showing *Mattrix OS 6.0 Professional Edition*, a small pretend Unix that
lives in memory. It has an intro, a shell, about fifty commands, a few full-screen
programs and some things to find. Nothing in it can touch the real computer: `rm -rf`
removes pretend files, `reset` or a reload brings everything back. How it works and
what is in it: [FAKEOS.md](FAKEOS.md). Spoilers: [FAKEOS_EGGS.md](FAKEOS_EGGS.md).

The same program, `src/fakeos`, runs in three places.

## In the browser (static, no server)

The demo is a set of plain files; any static host serves it, there is no
`matterm-server`, no WebSocket and no login. Build the web client (web/README.md) and
serve `index.html?demo`, or the generated `demo.html`, which is the same page
with a marker so no query string is needed. The files it needs: `index.html` (or
`demo.html`), `index.js`, `index.wasm`, `index.data`, `login.js`, `style.css`, and
`embed.js` for the snippet below. No inline script or style, no cookies, no secrets,
so a strict Content-Security-Policy is fine where the host allows setting one.

Parameters (`?nointro`, `?noreveal`, `?cmd=`, `?theme=`, `?embed`, `?smooth=`) are in
web/README.md.

### Embedding in a homepage

Plain iframe (simplest and isolated):

```html
<iframe src="https://example.org/matterm/demo.html?embed&nointro&cmd=neofetch"
        title="matterm demo" width="100%" height="420" loading="lazy"
        sandbox="allow-scripts allow-same-origin" allow="clipboard-write"
        referrerpolicy="no-referrer" style="border:0;border-radius:10px"></iframe>
```

Or let a small script build it from a `<div>`:

```html
<div data-matterm-demo data-src="https://example.org/matterm/demo.html"
     data-height="420" data-nointro data-cmd="neofetch" data-theme="dark"></div>
<script src="https://example.org/matterm/embed.js" defer></script>
```

Notes:

- `sandbox="allow-scripts"` is required (it is WebAssembly and JavaScript).
  `allow-same-origin` lets the demo remember its font size in `localStorage`; without
  it everything still works, it just forgets. `allow="clipboard-write"` is for copying
  a selection.
- The iframe gets the keyboard when it is clicked, as any iframe does.
- The page pauses itself while it is out of view, so a demo further down a long page
  costs nothing until it is scrolled to.
- The demo has to be hosted apart from `matterm-server`, which never allows framing
  (`frame-ancestors 'none'`): never frame a real terminal. The demo page has no
  login and no state worth stealing.
- `?embed` removes the window frame (the page around the terminal) and the toggle;
  `?theme=` only changes the page colors behind it.

## In a native window

```
matterm-demo [font.ttf] [--font-size N] [--no-intro] [--no-reveal]
             [--no-smooth-cursor] [--sound] [--seed N]
```

The terminal model and the SDL renderer, cell-snapped like the other frontends, with
mouse reporting for programs that ask for it, selection and the wheel. The cursor
glides by default; `--sound` plays film-terminal pips (SDL audio, off by default). `exit`
closes the window.

## In your own terminal

```
matterm-demo --tty [--no-intro] [--no-reveal] [--seed N]
```

The same pretend system in gnome-terminal, xterm or any terminal: raw mode, window
resizes followed, the terminal restored on exit. Colors follow `COLORTERM` and
`TERM` (truecolor, 256 or 16 colors), a console without the alternate screen
(`TERM=linux`) gets screen clears instead. It needs a terminal on standard input and
output and opens nothing else.

## The other demos

`matterm-render-demo` (the renderer alone) and `matterm-lib-demo` (the terminal model's
direct API) are separate examples of the libraries; `matterm-demo` is this one.
