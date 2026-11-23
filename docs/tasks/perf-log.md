# Lag in matmux/matterm over the network: a performance log, and a bug found on the way

Owner's report (2026-11-23): starting neovim made the terminal lag a lot, over the local network,
which should not carry much traffic. He wants a performance log that finds out where the time goes
(moving data, a loaded server, something else), and thinks automatic compression could help if
the network turns out to be slow. Measure first; compression only if the numbers ask for it
(`ssh -C` already compresses an ssh connection; no new compression dependency without a reason).

## Bug found by the integrator (fix first, agent3)
In passthrough mode (`matmux`, default) the hidden model (`TerminalView`, `events.reply`) answers
the program's terminal queries itself, while the query bytes also go to the host terminal, which
answers too. The program (neovim asks DA1, CPR, XTVERSION, OSC 11 ... at start) therefore gets two
answers, one of them matterm's own (wrong colours, own idea of the cursor position), and may wait
or misread. Reproduction (a program that sends `ESC[c ESC[6n ESC[>q ESC]11;?ESC\` in raw mode and
prints what it receives, started with `matmux --new -- python3 probe.py` under a pty that answers
nothing): it receives `ESC[?1;0c ESC[1;1R ESC]11;rgb:1e1e/1e1e/1e1e` from the model although the host
answered nothing. Fix: in passthrough the model never answers (only the host terminal does); in the
rendered mode and in the window the model keeps answering (it is the terminal there). A pty test
with that probe: with a host that answers, exactly one answer arrives; with one that does not,
none comes from matterm. Check that no replay (hidden attach phase) ever sends replies either.
Whether this is the whole neovim lag is not known: measure it with the log below, with and without
the fix.

## The performance log
A switch (`MATTERM_PERF_LOG=/path` or `--perf-log FILE` on `matmux`, `matterm` and
`matterm-daemon`) writes one text line per second (and a summary at exit), nothing when off,
no cost when off. What to record, where it matters most first:
1. **Echo latency**: the time from a key byte sent by the client to the first output bytes that
   come back (avg, p95, max per second). This is the number that feels like lag.
2. Round trip to the daemon (a ping on the control/terminal connection) next to it, so network and
   daemon can be told apart; with `--daemon-host` also the ssh tunnel.
3. **Bytes and messages** per second in and out on both ends; the size of the largest message.
4. **Daemon**: time per pump round (avg/max), bytes read from the pty per round, sessions, clients,
   how long a client was held back (backpressure/window), CPU time of the daemon process
   (`getrusage`).
5. **Client**: time spent parsing and drawing per second (matmux: in the hidden model; window:
   parse, layout, draw), how long writing to the host terminal blocked (stdout would block),
   stalls (`wantsRead` false), CPU time of the process.
6. A one-line explanation printed at exit: "most of the delay is: network | daemon | client |
   host terminal", from those numbers, as a hint only.
Tests first for the pure parts (the statistics, the verdict); a real-daemon pty test that runs a
chatty program (a script that redraws like an editor: cursor moves, colours, `seq` bursts) and
checks that the log has the lines and plausible values. Document the use in docs/CPU.md (how to
measure the neovim case: run `nvim` in a matmux terminal, read the log).

## Split
- agent3 (first): the reply bug and its test; then the client side of the log (matmux and the
  window) and the echo-latency measurement.
- agent2: the daemon side of the log (pump rounds, bytes, clients, CPU, ping), the log writer shared
  by both (a small header), and the verdict.
- Both: feature descriptions in `docs/handover/features/agentN-<name>.md`; SHAs to the integrator.
Rules as before. State what was not run (no real network here: use the loopback and, if possible,
a tc/netem-free way to add a delay in a test, e.g. a delaying relay in the test).
