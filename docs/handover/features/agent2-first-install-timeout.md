# First SSH installation: separate upload and readiness budgets

## Reproduction and fix

`SshTunnel::open` established its startup deadline before `installRemote`, which
has a separate installation timeout and can require several SSH commands. A
successful first upload could consume the entire startup allowance, causing the
subsequent remote startup/forwarding phase to fail immediately. Both forced
installation and automatic installation after a missing-daemon response shared
the defect.

Tests-first commit `8475543` adds a real local fake-SSH regression using the
existing `FAKE_SSH_DELAY=0.4` per-command delay. Installation takes longer than
the one-second startup budget but less than its independent 20-second test
installation budget. The test verifies that the uploaded file exists and that
both Always and Auto policies reach a real daemon over a real forwarded Unix
socket. Before the fix, it failed after 2.75 seconds with a successful upload but
a nonempty startup error. After the fix, both paths pass (6.89 seconds combined).
No new fake SSH implementation, injected clock, global timeout increase or host
connection is used.

The fix refreshes only the remote startup/forwarding deadline after a successful
installation. The initial no-install probe remains bounded by the original
startup budget. Installation errors return unchanged; upload verification still
uses its existing independent timeout. Remote startup and local forwarding share
the new bounded startup budget, rather than receiving unlimited retries or
resetting a deadline in their polling loops. Reconnect behavior is unchanged.

## Verification

`sshtunnel_test`: 22/22 cases pass, including existing refusal, architecture,
failed-executable, upload cleanup, relay and reconnect coverage. Target builds
without warnings. No real SSH host, service, lingering or systemd configuration
was changed. This is a separate bug fix from the staged managed-service work;
no systemd feature is required for the reproduction or fix.
