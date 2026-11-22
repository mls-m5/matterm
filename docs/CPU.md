# CPU use of the native window (2026-11-22, agent2)

The owner's native matterm sat at about 90 % of a core for hours (a build from 12:22, with Claude Code
running in the window).

## Cause

The native loop (`runPtyFrontend` in `app/ptyfrontend.cpp`) built and drew **every pane, every cell, in
every round of the loop**, whether or not anything had changed, and called `SDL_SetWindowTitle` each time.
With a display that waits for vertical blank that is 60 full redraws a second for an idle terminal; where
presenting does not wait (software renderer, some drivers) it is a busy loop at 100 %. A program that
redraws its status line 10-30 times a second (Claude Code's spinner and input box) only added output on top
of that. The web frontend already skipped unchanged frames (`FrameSignature`, `windowSignature`); the native
one never did.

## Change

* The frames are collected first (the model is advanced and its frame, selection, link underline, cursor
  glide and copy-mode badge computed as before), then hashed: `frontend::windowSignatures` gives `full`
  (every pixel, blink phase included) and `steady` (without the blink phase). `full` unchanged: nothing is
  drawn and nothing is presented. The window title is set only when it changed. The window size, the
  shared marks and the title are part of the hash; any SDL window event (exposed, resized, shown) forces
  a redraw.
* A round that draws nothing waits in `SDL_WaitEventTimeout`: input wakes it at once; 4 ms while something
  happened in the last 500 ms (`steady` changed or an input event), 100 ms otherwise. A round that draws
  waits until 8 ms have passed since it began (at most about 120 frames a second where presenting does not
  wait). Output from the programs is picked up by the next round, so an idle window shows output from a
  background program up to 100 ms late; typing is not affected (it wakes the loop).
* Blinking (cursor, SGR 5 text) stops, visible, after ten seconds without input or change (as GNOME's
  `gtk-cursor-blink-timeout`): a resting window then draws nothing at all instead of two frames a second.
  Any input or change starts it again.
* `FrameSignature::bytes` hashes a word at a time (the cells are hashed every round: that is the price of
  asking "did anything change"; about 8 times cheaper than byte by byte).
* `MATTERM_LOOP_STATS=1` prints "N loop rounds, M frames drawn in S s" at exit, for the next time a window
  uses CPU: rounds are many and frames few when it works, frames close to rounds when something keeps the
  window changing.

The lazy-reflow timer, the viewport animation and the cursor glide need no special care: they run in
every round (`advanceViewport`, `glide.update` before the signature) and change the frame while they have
something to do, which keeps the loop awake exactly that long. Nothing else wakes it: the tab strip, the
atlas and the daemon poll (every 2 s, 10 s over a tunnel) do not draw by themselves.

## Measured

`bench/cpu-measure.sh BIN SCRIPT [seconds]` runs the window headless (`SDL_VIDEODRIVER=dummy`,
software renderer, `--no-daemon`) and prints the CPU of the process over the window after a warm-up;
`bench/claudelike.py` is the synthetic load (a live five-line block redrawn in place with cursor up,
erase line, spinner, colours and a box, `RATE` updates a second, plus a coloured line scrolled above it
every fourth update); `bench/idle.sh` does nothing. Release build, before = main d4f6c1c.

| load | before | after |
|---|---|---|
| idle (after the blink timeout) | 99.9 % | 1.2 % |
| Claude-like, 10 updates/s | 100.1 % | 5.9 % |
| Claude-like, 30 updates/s | 100.0 % | 11.0 % |

Frames drawn while the load ran: 119 in 10 s at 10 updates/s and 316 at 30/s (one per update plus the
cursor), against about 60 a second before, always.

The remaining 1.2 % at idle is the dummy video driver: SDL's `SDL_WaitEventTimeout` polls every millisecond
for drivers that cannot wait (X11 and Wayland wait in `poll`, there it should be about 0). The remaining
5.9-11 % under load is the software rasterizer inside SDL (a profile of our own code, gprof, shows about
0.15 s in 12 s: `GlyphAtlas::getOrInsert` 33 % of that, `SdlBackend::render` 20 %, the signature 7 %); with a
GPU renderer a frame costs less. Possible later: `render` looks every glyph up twice per cell (once to
resolve misses, once to draw) and could keep the result of the first lookup.

## For things that animate (tab activity gradient)

An animation that changes the cells of the strip makes `steady` change and keeps the loop awake at
4 ms rounds, drawing whenever the frame differs: up to 120 frames a second. Change the content at a low
rate (10-15 times a second is plenty for a gradient), only while there is activity, and the cost is that
many frames. Do not animate by changing something every round.
