# Shared performance log contract

Owner: agent2 (`src/perflog.h`, daemon instrumentation). Agent3 owns native and
passthrough clients, client transport instrumentation and the perfclient adapter.
All code is opt-in: check `Writer::enabled()` before clocks, counters or samples.
No terminal content, key values, command lines or host names are logged.

## Shared API (`namespace perf`)

- `Samples::add(double milliseconds)`, `merge(const Samples&)`, `clear()`,
  `empty()`, `summary()`, `take()` (returns a Summary and clears). Do not use
  take() before run-total aggregation: merge the Samples into Second instead.
- Summary contains count, avg, p95, max. Count/sum/max are exact (subject to
  floating-point precision). Storage is bounded: 155 counters, independent of
  run duration. Zero is separate; positive values <=0.125 ms share a bucket;
  eight logarithmic buckets per octave extend through 65536 ms; higher values
  go into overflow. p95 is the nearest-rank bucket upper bound, explicitly
  `p95_approx_ms` in output. Above 0.125 ms bucket width is <9.1%; overflow
  returns observed max as a conservative bound. Negative/nonfinite samples
  are ignored. Client and daemon use identical semantics.
- `Second`: Counters counters; Samples echo,rtt,pump; double cpuMs;
  uint64 sessions,clients (end-of-interval gauges).
- Counters: bytesIn/Out, messagesIn/Out, largestIn/Out, stalls, ptyBytes,
  parseMs, layoutMs, drawMs, blockedMs, backpressureMs; in/out count one
  protocol frame including the four-byte framing prefix (not socket chunks).
- `Totals::add(Second, intervalSeconds)` merges histograms; total(),
  elapsedSeconds(), seconds() (number of intervals, including partial ones).
- `Writer(path, role)`: enabled(), error(), add(Second, intervalSeconds),
  finish(). Empty path uses MATTERM_PERF_LOG. Caller owns scheduling and must
  submit the last partial interval before finish. finish is idempotent;
  destruction calls finish but cannot collect unsubmitted caller counters.
  CPU is process CPU milliseconds divided by actual wall interval.
- Append-only output, one write syscall per line (EINTR retried), role and PID
  on each line; open/write errors disable collection and remain in error().
  Files created with mode 0600; existing permissions are not changed. No
  background thread, implicit clocks or flushing timer. Regular-file writes
  can themselves block on slow storage: use a local diagnostic file.
- Verdict is only a heuristic for locally dominant measured work (>50% of
  wall time): client, host terminal, daemon, otherwise unknown. RTT alone
  never establishes network versus daemon delay. No protocol extension or
  remote metric aggregation is implied by this header.

## Verification so far

Tests-first contract commit intentionally failed compilation without the header.
`build/test/perflog_test`: four cases pass (bounded/merged histogram, overflow,
actual interval CPU weighting, partial interval output and idempotent finish).
Daemon instrumentation now records execution-only pump durations, fresh PTY bytes
(total and average/max per pump), admitted outgoing / decoded incoming framed
messages, CPU and session/client gauges. The existing Ping/Pong works unchanged;
RTT belongs to client instrumentation (no new control version).

The daemon CLI supports --perf-log and its completion table is updated.
Registry::tick takes an optional TickMetrics pointer (default nullptr), resets
it per call, and reports fresh ptyBytes and windowBlockedSessions. Replay is
not counted again. Zero budget is only an observation: the daemon integrates
the previous count over the interval between pump starts into
windowBlockedSessionMs, logged as window_blocked_session_ms_sampled. It is not
actual PTY blocking time. backpressureMs logs as socket_write_pending_ms and
tracks asynchronous socket writes, split at report boundaries, not proven
kernel congestion. largestPtyRound captures maximum fresh bytes per tick.
Incoming bytes exclude malformed/incomplete frames; outgoing bytes are counted
at queue admission, not after acknowledgement. Regular and partial-interval
CPU use actual wall time.

Verification: full warning-free build; perflog + daemon/registry/replay/session/
lifecycle regression selection 11/11 passed, plus the additional exhausted-window
metric regression. The first broad run passed 135/137 (excluding matmux_pty and
soak); cli_options correctly caught the missing daemon completion entry (fixed),
and fakeos needed rebuilding after test additions; both subsequently pass.
The standalone matmux_pty run failed four existing list/EndSession interaction
assertions (program name/yes/end/removal); the passthrough reply probes passed.
Do not infer that end-to-end list integration is verified. Focused clang-tidy
reports no diagnostics. Both environment and explicit flag daemon CLI smoke
runs wrote periodic, partial and summary lines. Real LAN/SSH/neovim/display
measurements remain unverified. No submodule or protocol changes.
