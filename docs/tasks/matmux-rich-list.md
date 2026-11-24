# matmux: three-line terminal entries

Owner decision: variant B. Each terminal occupies three visual lines; selecting
it highlights the entire block with a background colour. Colour must not be the
only selection cue. Use a persistent `>` marker and reverse video in monochrome
where supported; the marker remains sufficient when attributes are unavailable.

## Layout and semantics

1. Terminal name / folder name, foreground program.
2. Folder, foreground-program status, attachment status and recent-output age
   when known. Distinguish a running foreground program, recent output, and an
   attached client. A shell in the foreground is not proof of an idle prompt.
3. One bounded, safe text preview of recent terminal output. Never execute or
   forward terminal control sequences from previews. Do not call it the last
   command, and do not claim it is necessarily the final visible screen row.
   The exact inexpensive preview source must be agreed before backend work.

Retain New terminal, digit shortcuts, navigation, attach/takeover warning, own
terminal protection and end confirmation. Scrolling and page navigation operate
in entries, not visual rows. Clip long values by display width. For small windows,
show fewer entries or degrade gracefully rather than hiding the selected entry.
Older daemons show unavailable metadata as unknown, without breaking the list.
No terminal may be attached/taken over merely to collect preview information.

## Ownership and staging

- agent3: frontend model, three-line layout, colour/monochrome selection,
  navigation and frontend/PTY tests. Reuse the existing tty rendering/input layer.
- agent2: first inspect an inexpensive, bounded source for preview and last-output
  age; propose metadata/API and compatibility details before changing the daemon
  protocol. Reuse existing storage/parsing helpers, do not build a second VT parser
  or a full terminal model per list entry. No unbounded output history or logging
  of preview contents. Keep ordinary output handling cheap.
- Integrator: approve the shared metadata contract, review/integrate, update shared
  documentation and coordinate test registration. Agree exact shared file ownership
  before parallel edits.

Preview text is potentially sensitive. Provide a way to disable it without
removing the three-line selection/navigation improvements. Do not add a setting
outside the existing settings model independently of the other agent.

## Acceptance

Tests first: all three selected lines highlighted; visible selection without
colour/attributes; entry navigation/page sizing at multiple heights; wide UTF-8,
control/OSC sequences and long preview sanitization; unavailable/old-daemon data;
no attachment side effects from list refresh. Existing terminal-end and fresh
startup PTY tests remain green. Use deterministic activity ages in unit tests and
bounded polling/caching, not busy redraw loops. State what was not visually
verified. Separate backend metadata and frontend layout commits.
