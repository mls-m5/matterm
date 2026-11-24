# SSH startup phase investigation

## What is known, and what is not

A reported pause at `matterm: connecting to ...` is not enough to identify DNS,
authentication, installation or daemon work. The owner has been asked for the
exact command/server and whether this happens every time or only on first
installation. No real host was contacted in this investigation.

The normal already-installed path launches **two sequential SSH processes**:

1. `ssh ... DAEMON --ensure --print-socket`: this combines SSH connection setup,
   DNS/config/authentication, remote shell execution and daemon ensure/start.
2. `ssh -N -L ...`: forwarding setup, which may repeat SSH connection setup unless
   the owner's existing SSH ControlMaster configuration reuses it.

`startForward` currently tests only whether the local socket accepts a connection.
It does **not** complete a remote protocol handshake. The subsequent
`selectDaemon` performs ControlWelcome/GetStatus to establish the real daemon
identity and compatibility. This distinction is now visible in phase names.
Removing that probe on the basis of the socket accepting connections would lose
a correctness/safety check. There is no redundant standalone remote version SSH
process on the ordinary no-install path; the fake test asserts exactly two SSH
invocations. Later pane/list actions have their own required control requests.

A forced install or missing-daemon fallback also performs local hashing/version
inspection, remote architecture/directory/presence checks for daemon and matmux,
optional user confirmation, uploads with executable/hash verification, stable
name linking and courtesy cleanup. They are sequential today. Cleanup scans
`/proc/*/exe` for versioned installed files to avoid removing running binaries;
on a host with many processes or installed versions, that is a plausible cost
but is **not measured on the reported host**. Merging link/cleanup calls or
batching both program probes may save SSH startups, but no such optimization is
claimed or implemented before phase evidence is available.

## Opt-in diagnostic interface

Use the existing `--perf-log FILE` (or MATTERM_PERF_LOG). No new dependency or
host setting is needed. With diagnostics enabled, startup prints a static phase
name before blocking and appends begin/end performance records, for example:

```
matterm-ssh-startup pid=123 event=phase phase=ssh_remote_ensure phase_ms=0.0 status=begin
matterm-ssh-startup pid=123 event=phase phase=ssh_remote_ensure phase_ms=240.0 status=ok
```

Only audited literal names are accepted by `perf::Writer::phase(name, elapsedMs,
success=true, begin=false)`. Arbitrary names, negative/nonfinite times are
rejected. Records contain no host, argv, authentication text, shell output,
terminal content or remote path. Existing error/progress reporting is unchanged;
the new timing callback does not copy it into the performance log. Log files
still follow the existing performance-log permissions/append behavior.

Observed boundaries:

- `ssh_startup`: aggregate tunnel opening, not GUI presentation or attachment.
- `ssh_remote_ensure`: one SSH command including opaque SSH connection/auth and
  remote shell/daemon ensure; not a measurement of daemon-only startup.
- `ssh_forward_socket`: forwarding process spawn until local socket accepts;
  NOT end-to-end handshake/readiness.
- `ssh_control_probe`: the real ControlWelcome/GetStatus compatibility probe.
- `ssh_installation`: aggregate install work (nested phases must not be summed
  with it).
- `ssh_local_identity`: local executable `--version` subprocess, not the preceding
  whole-file SHA computation.
- `ssh_remote_inspection`: remote architecture/path/presence SSH command.
- `ssh_confirmation`: time awaiting the existing installation confirmation.
- `ssh_upload`: SSH transfer plus remote validation/rename, not bytes-only wire
  time.
- `ssh_install_links` / `ssh_install_cleanup`: auxiliary remote commands, including
  their SSH setup; cleanup may produce more than one record.

For command subphases, `ok` means the subprocess completed successfully; a
subsequent semantic validation can still fail the containing installation/startup
phase. A failed courtesy-cleanup command need not fail the overall connection.

These are locally observable boundaries. DNS, key exchange, authentication and
remote CPU cannot honestly be split from each other with the current subprocess
interface. SSH verbose output may contain sensitive information and is NOT
captured in these records. Existing ssh arguments/config still control transport
connect/auth options; an owner can inspect their own SSH configuration and
connection reuse separately. This patch does not silently alter multiplexing,
credentials, host verification or authentication policy.

No timing clock is read when the callback is absent. Diagnostic callback failures
are isolated, including during stack unwinding. Callback captures used for app
logging own their writer for the tunnel/reconnect lifetime; no reference to a
startup stack variable escapes. Off performs neither phase log writes nor extra
SSH/RPC requests. The independently integrated post-upload readiness budget fix
is preserved, not replaced with a larger general timeout.

## Verification checkpoint

Tests-first: a824aa9 specifies successful/failed paired scopes, throwing logger
isolation and exactly two normal-path SSH launches. The existing fake gains
separate ensure-command and forwarding-setup delays. A 200 ms ensure delay and
400 ms forwarding delay are distinguished with minimum-bound assertions, not
assumed to represent DNS/auth substages. The slow successful installation test
also checks that each install subphase is emitted under Always and Auto policies.

Formatter tests-first: 71de767; shared Writer implementation 0e55395. The same
strict API includes the separately requested settings_factory,
settings_first_parsed_output and settings_first_present names for agent3's client
wrapper; this task does not alter settings or GUI behavior.

Final verification: **147/147 tests pass**, 106.44 seconds with four workers,
excluding only the two previously unsafe-cleanup layout smoke tests. Full build
is warning-free. SSH covers 24 cases, performance logging 7; the native performance
log smoke test passes. Focused clang-tidy on SSH phases/tunnel emits no user
diagnostics. This uses the agent branch's preserved pre-attribution-rewrite base;
no merge/rebase/reset of the rewritten main was performed. Native smoke covers
the existing local log path, not an actual remote host or real SSH authentication.
No host DNS/auth measurement, actual SSH server timing or real systemd verification
has been performed. Those require owner-provided reproduction details and separate
host authorization where applicable.
