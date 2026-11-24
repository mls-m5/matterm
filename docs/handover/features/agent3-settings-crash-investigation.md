# Settings native-window crash/slow-start investigation (still open)

The user reports intermittent whole-window SIGSEGV when opening Settings from
the palette, especially a second opening after one successful mouse interaction,
and slower startup than matmux --settings in a normal terminal. **Neither the
crash nor the slow startup has been reproduced here. No speculative product
crash fix is claimed.** Exact user close method and desktop binary/core/stderr
are still needed; GPU/physical desktop behavior was not exercised.

Reproduction improvements: `ca61ca5` uses real SDL TextInput(Settings)+Enter
through the palette in three tabs with two splits each. `a8417cd` extends this
in the SAME native process: open/save/quit the local TUI, reopen/save, confirmed
Close Tab, then open a third time. The test also covers direct action launch,
local daemon and --daemon-host with an owned SSH relay. It uses temporary local
configuration and never modifies the user's files. The injected factory-failure
regression still verifies the nextSettings override is cleared.

Normal binaries pass all four combinations. A Debug AddressSanitizer+UBSan
build with no-PIE also passes repeated launch/close/relaunch against a real
owned daemon. The initial sanitizer PIE binary intermittently printed only
DEADLYSIGNAL at startup, even on noninteractive invocation; no-PIE eliminated
that test-environment problem. This is not evidence that the user's crash has
the same cause. No sanitizer report/backtrace of the reported crash obtained.
A temporary child-exit terminal-text diagnostic was removed before commits.

One smoke-test false failure was found: an earlier direct stale-detach action
bypassed the ConfirmDialog's normal pop and left a test-only overlay. Explicitly
closing that test layer restores the real action/layer lifecycle. This is not
a product crash fix.

## Audited phase timing

`18d6202` tests-first / `8b46f3a` wiring use agent2's shared Writer::phase
whitelist API (`71de767`, `0e55395`). With --perf-log FILE, Settings launch emits:

- settings_factory begin/ok/failed: action through local factory/forkpty and
  exec-status pipe completion; includes workspace insertion, not just fork.
- settings_first_parsed_output: action to first completed output pump including
  parsing; not the exact first read syscall timestamp.
- settings_first_present: action to first backend presentation containing parsed
  Settings output.

All names are static/audited; no typed text, terminal contents, path or command
line is recorded. No per-pane callbacks retain a destroyed NativePane. Timing
state is local scalar fields; disabled diagnostics do not read a clock.

On the local normal build, eight launches (including local/tunneled and repeat
opens) measured factory 3.3-4.2 ms, first parsed output 13.3-16.1 ms, first present
22.7-25.5 ms. These dummy/software-renderer results do not reproduce the user's
slow startup. In the child spawn implementation, close_range fallback can scan
_SC_OPEN_MAX descriptors before exec and the parent waits on an exec-status
pipe; this is a branch worth checking if factory timing is high on the user's
host, not an established cause. Child allocation/environment/PATH preparation
is already done before fork; no new thread/fork mechanism was introduced.

Next occurrence: run the exact desktop binary with --perf-log FILE and retain
stderr/core plus version/build SHA. Confirm how Settings was closed and whether
the GUI itself or only the Settings child crashed. Compare phase timings with
ordinary matmux --settings. A real desktop backtrace is still required before
claiming a crash fix.

Validation including scoped detach changes: full warning-free native build;
148/148 tests pass, 122.76 seconds, excluding only two old prohibited-pkill
layout scripts. ASan/UBSan-noPIE repeated lifecycle test passed before the
phase-only wiring. Physical desktop and real SSH/network/GPU are limitations.
