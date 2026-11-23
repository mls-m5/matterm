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

## Replay: reattaching to a terminal with a full ring (2026-11-23, agent2)

The owner saw a pane scroll through its history for a while after a reattach. Measured with two tools, both in
the repository (Release build, `-DMATTERM_BUILD_BENCHMARKS=ON`, the machine of the other numbers here):

* `matterm-replay-bench [--mib N] [--round-ms M] [--frame]` (`bench/replaybench.cpp`): an in-process daemon, a
  terminal that printed N MiB of coloured log lines (the ring is full), and a client (`DaemonLink` +
  `TerminalView`, the same classes the window and matmux use) that attaches from the start of the ring. It
  reports wall time, bytes per round of the client's loop, and the time spent in the client's tick (transport,
  framing, acknowledgements, parser, model) against the time spent waiting. `--round-ms` imitates the window's
  loop cadence, `--frame` adds the cost of hashing the frame every round. `--restore K` instead times K panes
  being made and attached one after the other, as a session restore does.
* `bench/replay-measure.py BUILD_DIR [--mib N]`: the real window, headless (dummy video driver, software
  renderer), whose first pane attaches to such a terminal (the hidden `MATTERM_ATTACH_TERMINAL=<id>`
  environment variable); it reports the time until the window's CPU use settles, the CPU time used, and
  `MATTERM_LOOP_STATS` (rounds and frames drawn).

### Where the time went (before; ring 3.2 MiB)

| | |
|---|---|
| client: transport + parse + model for all of it | 0.05-0.07 s |
| hashing the frame in every round (window loop) | 0.01-0.5 s depending on the round rate |
| the replay as a whole, in the bench | 2.05 s (1.55 MiB/s), the same at any client round rate |
| the replay in the window | settles after about 10 s, 1.4 s CPU, 518 rounds, 236 frames drawn |
| 32 panes made and attached (restore) | all showing output after 0.20 s: 2 ms per pane to ask the daemon, the rest is the shells starting, in parallel |
| attaching when the output is already in the ring | 10 ms to the first output |

The client is not the bottleneck: the parser takes a few hundredths of a second for the whole ring. The
**daemon sent the replay at 16 KiB per 10 ms** (one message per entry per pump tick, `SessionEntry::pump`),
which is 1.6 MiB/s however fast the client is, and **every one of those chunks made the window draw a frame**
(a software-rendered frame is tens of milliseconds): about 200 frames for 3 MiB. Restoring many panes is
bound by starting the shells, not by replay.

### After (the daemon sends the window's worth at once; a flood is drawn at most ten times a second)

The registry now sends a replay up to the output window (256 KiB) per pump instead of one message, and the window and
matmux draw at most every 100 ms while more than 32 KiB arrive per round (input, a resize or an expose draw at once;
`ptyfrontend.cpp` "flooded", `ttyattach.h` `kFloodDrawMs`). Same ring (3.2 MiB), same machine:

| | before | after |
|---|---|---|
| replay in the bench (`--round-ms 8`) | 2.07 s, 245 rounds | 0.17 s, 12 rounds |
| client's own time (transport + parse + model) | 0.05 s | 0.05 s |
| window, CPU in the first 3 s (idle window: 0.07-0.18 s) | 1.17 s | 0.17 s |
| window, frames drawn in 3 s (idle window: 7-14) | 217 | 13 |

The parser was never the cost; the rest was waiting for the daemon and drawing every chunk. Over a slow link (ssh) the bytes
themselves take the time: that is what a checkpoint would save (docs/tasks/replay-performance.md step 3).
Reproduce: `cmake -DMATTERM_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release`, then `matterm-replay-bench --mib 4 --round-ms 8`
and `bench/replay-measure.py BUILD_DIR --mib 4` (`--mib 0` gives the idle baseline).

## Opt-in daemon performance log

Start the daemon with `matterm-daemon --perf-log /tmp/daemon-perf.log` (or
`MATTERM_PERF_LOG=/tmp/daemon-perf.log matterm-daemon`). Logging is off by
default. The native client is instrumented separately by agent3; the reported
case is **nvim in a native matterm window via --daemon-host on the LAN**, not
matmux in a host terminal. To reproduce, enable the client log, enable the
remote daemon log on that computer, attach the native window, then start nvim
and type/scroll. Record both logs during the same interaction. Setting the
client environment does not configure a daemon already running remotely;
`--ensure` reusing an existing daemon also does not turn on its logging. Do not
stop a daemon with valuable live sessions merely to enable a measurement.

The daemon writes a line about once per second, a final partial interval, and
a summary. Roles and PIDs distinguish processes; elapsed times are local, not
synchronized timestamps. Lines contain counts/timings only, never terminal
contents or keystrokes. Use a local regular file; logging itself can cost I/O.
CPU is process milliseconds / actual elapsed interval, so may exceed 100% for
multithreaded processes. Bytes/messages count admitted outgoing and decoded
incoming protocol frames including the 4-byte length prefix, not SSH/TCP bytes
or acknowledged delivery. PTY bytes count fresh reads only (not replay);
per-pump average/max and pump execution time exclude the 10/200-ms timer wait.

`window_blocked_session_ms_sampled` integrates the previously sampled number
of Running sessions with zero output budget over the interval between pump
starts. It can exceed wall time for multiple sessions and does **not** prove
output was waiting in the PTY. `stalls` counts such session observations.
`socket_write_pending_ms` sums outstanding async socket-write durations across
clients (split at reporting boundaries), not time proven blocked in the kernel.

Statistics have bounded memory for arbitrarily long runs: 155 buckets per
sample family, eight logarithmic buckets/octave from 0.125 to 65536 ms, with
zero and overflow. Count/average/max use the original values; `p95_approx_ms`
is the selected bucket upper bound (<9.1% bucket width above 0.125 ms;
overflow uses observed maximum). Summaries merge buckets, never percentiles.
The exit verdict is only a hint, and often `unknown`: RTT alone cannot
separate network latency from daemon scheduling. Existing Ping/Pong is used
without changing protocol versions; client RTT must remain asynchronous.

Verified with a real local daemon/PTY and editor-like output, existing ping,
partial-interval flushing, and registry replay/window regression tests. No
real LAN/SSH/neovim or display measurements have been performed here.
