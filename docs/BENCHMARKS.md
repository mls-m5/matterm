# First Performance Measurements

These are uninstrumented baseline tools. They do not enable matperf yet; its
concurrency caveats are recorded in SETTINGS_AND_PERFORMANCE.md and its own README.
No per-cell/per-byte tracing is added. Both tools use the same prebuilt workload
bytes, generated before timing, with no filesystem traversal or random generation.

## Release Build

```sh
cmake -S . -B build/perf -DCMAKE_BUILD_TYPE=Release \
  -DMATTERM_BUILD_BENCHMARKS=ON -DMATTERM_BUILD_APP=OFF \
  -DMATTERM_BUILD_DEMO=OFF -DMATTERM_ENABLE_PTY=OFF
cmake --build build/perf --target matterm-parser-bench matterm-terminal-bench
```

Benchmarks are opt-in. The external tool currently requires POSIX and is not built
for Emscripten. A library/bench-only build does not download a font.

## Parser/Model Without I/O

```sh
./build/perf/matterm-parser-bench long 2097152 4096 5
```

Arguments: workload, approximate bytes, feed chunk bytes, repetitions. Workloads
are `long` (78-character lines), `short`, `color`, and `redraw` (cursor-addressed
24-row frames). Bytes round up to a complete workload unit and the exact count is
reported. Each sample constructs a fresh 80x24 model with 1,000 history rows before
timing, feeds all bytes, and finishes decoding. One unreported warm-up precedes
samples. Reported median is the upper middle sample for even repetition counts.
A checksum of final screen/style plus history size is consumed outside timing so
the compiler cannot simply discard all terminal work.

This measures parser/model wall time only: no PTY, SDL, atlas, GPU, or display.
For live terminal comparisons, use the external tool, not these numbers.

### Initial Sandbox Baseline

GCC 12.2.0, CMake Release, tracing off, 80x24, history 1000, 2 MiB approximate,
4096-byte chunks, one warm-up, median of five. Shared sandbox CPU/load is not
controlled; these are starting observations, not stable performance guarantees.

| Workload | Exact bytes | Median ms | MiB/s |
|---|---:|---:|---:|
| long | 2097200 | 39.76 | 50.30 |
| short | 2097180 | 48.64 | 41.12 |
| color | 2097157 | 29.85 | 67.00 |
| redraw | 2098023 | 36.33 | 55.07 |

No optimization conclusions or comparisons against GNOME Terminal follow from
these internal-only samples. The color stream contains more control bytes and
fewer cell writes per MiB, so its throughput is not directly comparable to ASCII.

## External Terminal Comparison

Run the same binary inside matterm, GNOME Terminal, and other terminals:

```sh
/path/to/matterm-terminal-bench long 2097152 4096 --barrier
/path/to/matterm-terminal-bench short 2097152 4096 --barrier
/path/to/matterm-terminal-bench color 2097152 4096 --barrier
/path/to/matterm-terminal-bench redraw 2097152 4096 --barrier
```

Use identical grid sizes (80x24 recommended), font/size, history limits where
possible, machine/load, and chunk sizes. Record emulator version, rendering backend,
VSync, build type, and whether tracing was enabled. Avoid shell startup/config costs
inside timing. Repeat runs and compare distributions, not one sample.

Reports go to stderr after timing:

- `write_ms`: producer time until stdout accepted all workload bytes. Backpressure
  is included, but terminal rendering may still be pending.
- `dsr_processed_ms`: optional time through the final cursor-position reply. This
  is a processing barrier, not proof that final pixels reached the monitor.
- `display_complete=unknown`: no physical display acknowledgment is claimed.

`--barrier` requires tty stdin/stdout. It performs an untimed preflight query,
reads a bounded CSI row;column R reply with a 3-second timeout, writes the workload,
and repeats the query. Canonical input/echo are temporarily disabled and restored
through RAII on success/errors and handled INT/TERM/HUP interruptions. Don't type
while measuring: unrelated input can be consumed. SIGKILL/crash cannot guarantee
terminal restoration; `stty sane` recovers a disturbed terminal. Do not run it
inside another input-owning full-screen application.

Without `--barrier`, only write completion is measured, and stdout may be redirected
for a transport/file baseline. That is explicitly not terminal rendering throughput.
Writes use normal blocking stdout; the 3-second timeout applies to replies, not a
hung stdout consumer. Output contains fixed known terminal sequences, no filesystem
or external data injection.

## Verification and Next Steps

CTest smoke-runs each parser workload with tiny chunks. A controlled pseudoterminal
fixture verifies fragmented DSR replies, timeout handling, and termios restoration
without depending on a graphical emulator. The normal rendering/PTY suites remain
separate.

Next: collect comparable external samples on a real display and trace coarse parser,
session, geometry, and present stages after matperf synchronization review. Measure
cold/warm atlas behavior separately. Only then decide whether parser batching,
frame scheduling, or a parallel preparation pipeline is the first bottleneck.
