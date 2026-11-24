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

## Approved metadata direction

Integrator approved agent2 proposal `51917c9` (design document on agent2) after
review: additive control 1.3.5 with bounded batched details and unchanged legacy
messages; optional foreground classification, last retained PTY-read age and text
preview. Preview extraction reuses the EXISTING VtParser lexer in an opt-in,
model-free mode without semantic expansion/replies. Review this parser change
separately before backend integration; ordinary parsing must remain unchanged.

Decode only on request: at most 16 KiB per ID, 64 KiB/s daemon-wide, 16 LRU
entries, five-second inactive expiry, bounded text/lexer state. Respect the
existing 16 KiB frame limit with processed-count continuation. Cold tails with
unknown control-string state remain unavailable until safe synchronization;
never strip escapes from a raw tail or assume LF/BEL proves ground state.

`terminalListPreview` is approved as a client setting, default true, owned by
agent2. Disabled means no preview decoding/transmission and clearing displayed
cached text; it does not erase the daemon's existing replay history. Older
daemons still provide the list with unavailable optional metadata. Availability
and decoding cost must be verified, not assumed from byte limits.

## Acceptance

Tests first: all three selected lines highlighted; visible selection without
colour/attributes; entry navigation/page sizing at multiple heights; wide UTF-8,
control/OSC sequences and long preview sanitization; unavailable/old-daemon data;
no attachment side effects from list refresh. Existing terminal-end and fresh
startup PTY tests remain green. Use deterministic activity ages in unit tests and
bounded polling/caching, not busy redraw loops. State what was not visually
verified. Separate backend metadata and frontend layout commits.
