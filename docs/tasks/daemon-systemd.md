# Managed daemon startup with systemd

Owner request: an easy way to install a systemd service that starts the daemon
automatically after boot, restarts it after crashes, and uses the latest daemon
version successfully installed over SSH. Local versus remote scope can be
selected during setup; remote deployment is an explicit use case. Design first;
do not interrupt the ongoing rich-list implementation.

## Proposed direction to validate

- Prefer a per-user service, running in the foreground (no --daemonize), not a
  root daemon. Provide an explicit install/status/uninstall workflow without
  shell startup-file edits. Reuse existing SSH installation/path helpers.
- A user service normally starts at login. Explain that true unattended boot
  startup requires user lingering; detect/report its state and obtain explicit
  approval before changing host configuration. Do not silently enable it.
- Maintain an atomically updated stable launcher or pointer to the most recent
  successfully installed daemon. Never point at an incomplete upload. Define
  what successful installation means, verify that a candidate can run, and
  preserve a recoverable previous target. Do not execute an untrusted PATH match.
- Existing uploads use versioned binary names and daemon sockets. Inspect how
  version discovery and socket selection work before choosing ExecStart or a
  stable alias. Keep old clients' behavior and explicit socket overrides intact.
- Idle shutdown is intentional in the current daemon. A managed persistent
  service likely needs idle exit disabled, Restart=on-failure and rate limiting;
  validate the exact flags and exit semantics before writing a unit.

## Session safety and unresolved policy

Replacing a binary/pointer does not upgrade the running process. Automatically
restarting a daemon with active PTYs can terminate users' programs; a restart
after a crash cannot reconstruct lost PTYs either. Do not promise session
survival across daemon restarts. Propose safe upgrade behavior (defer switch until
no sessions, or run versioned instances side by side) and ask the owner before
adding any disruptive automatic restart. Binary cleanup must retain the running,
managed and rollback targets.

The owner has requested latest-upload behavior, not approved killing existing
sessions to achieve it. Define whether the newest successful upload wins even
when its protocol version is older; do not confuse upload order with version
comparison. Consider concurrent SSH installs and atomic state updates.

## Acceptance outline

Unit tests for launcher selection, quoting, atomic update/failure rollback,
version/socket matching, managed idle-exit behavior and cleanup protection;
installation dry-run/unit-file validation where available; tests for preserving
active sessions during an update. Actual user-service, lingering, SSH and boot
behavior must be verified on an authorized real host, not claimed from sandbox
unit tests. Host installation/enabling is a separate explicit approval step.
