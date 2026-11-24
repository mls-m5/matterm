# Primary-screen matmux host restoration

Tests first `0c943b0`; fix `cf520e3`.

The existing tty::hostRestore(false) first resets input modes, scroll margins,
origin, character set, pen and cursor visibility as before, then moves to the
host's last row and emits CR/LF. CUP uses a saturating row (65535), so the host
clamps to its actual current height without relying on stale daemon dimensions
or requesting a cursor report. A fresh line scrolls the former bottom row up
instead of writing a detach message over it. No erase/clear sequence is added;
primary history/scrollback remains available. There is one intentional scroll
when already at the bottom, so subsequent shell/list text starts below output.

Alternate-screen restoration is unchanged: leave 1049, then the existing CR/LF
at the saved primary cursor. This is the existing common restoration path, so
primary detach, process exit, takeover/connection loss and signal cleanup all
receive the same safe placement; no separate escape-sequence framework or
transport behavior is introduced.

Verification: tty snapshot tests resize a host from 40x30 to heights 1, 3 and
24, verify bottom cursor placement and preservation of the former last row
(in history for a one-line terminal), and check alt restoration has no added
CUP. Existing mode/margin restoration and alternate-screen tests pass.
Real-daemon matmux_pty runs primary output with a restricted scroll region,
resizes to 20x3, detaches and checks reset-margin -> bottom CUP -> CR/LF before
any message, without clearing or alternate-screen switching. Existing real
alternate-screen leave and SIGTERM restoration cases pass. Three targeted
suites (tty_snapshot, passthrough, matmux_pty) pass, 46.18 seconds; changed C++
formatted and build warning-free. No physical terminal visual check performed.
