# Client performance diagnostics (in progress)

The reported lag is neovim **inside the native matterm window** connected with
`--daemon-host` to another LAN computer. The passthrough terminal-query fix is
separate and is not evidence of a GUI lag fix.

`matterm --perf-log FILE --daemon-host HOST` records native-client measurements;
`MATTERM_PERF_LOG=FILE` is an alternative. `matmux --perf-log FILE` measures
passthrough; adding `--rendered` measures the rendered terminal path. Both use agent2's single shared writer/statistics/verdict in
`src/perflog.h`, with bounded histograms, approximate p95 and a final partial
interval. No terminal contents or key bytes are logged. Measurement clocks and
sampling callbacks are gated off when diagnostics are disabled.

Native measurements: per-pane successful user-input to first-output echo;
parser wall time; frame preparation/layout and workspace tick wall time; drawing
including present (but not the deliberate sleep); process CPU; terminal-protocol
message counts and encoded bytes including the Unix framing prefix; active
terminal-connection Ping/Pong RTT. Wire counts exclude daemon control/list requests
and SSH's own framing, encryption and compression overhead. RTT includes client
polling, daemon scheduling and the tunnel: RTT alone does not prove a slow network.
Echo is a heuristic: unrelated output can satisfy it. Query replies do not start
an echo sample. One-second intervals can be longer if synchronous work blocks.

Passthrough no longer counts parser chunks as messages. Nonblocking stdout
EAGAIN waits include the subsequent poll wait. A successful write's wall time is
also recorded. The shared verdict can be `unknown`; it is only a hint.

Validation: native headless test with a real daemon passes locally and through
`--daemon-host` with a relay injecting 30 ms in each direction; its RTT assertion
observes that delay. Disabled diagnostics and an unwritable destination also pass.
Final full native build passes without warnings. After the help-entry and PTY
helper corrections, ctest passes **137/137** (118.5 seconds, four jobs), including
perf_client, client_session, passthrough, tty_attach, shared perflog, complete
matmux_pty and native_perf_log. The two older layout smoke scripts were excluded
because their cleanup uses `pkill -f`; the new test exercises the built-in layout
smoke locally/remotely with owned-process cleanup.
The PTY flake was a test helper limited to rows 1-9 while more sessions existed;
walking every row fixes it. Complete matmux_pty now passes, including real-daemon
passthrough/rendered diagnostic files, summaries and immediate startup queries.
Rendered mode records parse, canvas preparation, tty presentation, echo, protocol
traffic, RTT and CPU; its presentation time includes synchronous terminal writing
rather than claiming a separate blocked-write measurement. No real
LAN/neovim/display/phone validation yet. clang-tidy could not run:
run-clang-tidy and clang-tidy are absent.
