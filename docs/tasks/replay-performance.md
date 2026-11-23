# Replay: measure, batch, checkpoint on detach (agent2)

Owner's notes (2026-11-23): when data is replayed after a reattach, the pane scrolls through it
and takes a while. Wishes: some performance measurements; see whether start-up time when
restoring sessions can be saved by batching; and make a **checkpoint when a terminal is
detached** so a reattach does not have to replay so much.

Order (stop and report after step 1, the owner decides before step 3)
1. **Measure.** Reattach to a daemon terminal with a long history (the replay ring is full) and
   restoring a session with many panes, both with a local daemon and with `--daemon-host`
   (fakessh is fine for relative numbers). Where does the time go: daemon sending, transport,
   parsing, model, drawing? Frames drawn during the replay? Write the numbers and the method to
   `docs/CPU.md` (a short section) and reproducible with a script or a hidden test option;
   `MATTERM_LOOP_STATS=1` exists.
2. **Batch.** While a large batch is applied (replay, many panes at once): apply to the model
   without drawing, draw once at the end (or at a low rate). Keep input and resize responsive;
   no behaviour change otherwise. Show the before/after numbers. `matmux` (src/frontend/ttyattach.h
   and the tty Screen) should draw once after a replay too.
3. **Checkpoint on detach (needs the owner's decision first).** Write the design question to the
   integrator with 2-3 alternatives and a recommendation: what is stored (serialized screen as
   escape sequences, scrollback or not), where (in the daemon's memory next to the ring only, not
   on disk, as a first step), when (at detach, and maybe periodically), how a client uses it
   (replay from the checkpoint instead of from the ring start), size limits, and privacy (the
   screen can hold anything that was shown, including echoed passwords; nothing is written to disk
   in this step). docs/MULTI_TERMINAL.md section 12 has the earlier ideas.

Rules as before: tests first, small commits, tidy clean, feature description in
`docs/handover/features/agent2-replay-<step>.md`, SHAs to the integrator.
