# One JSON reader instead of three (agent2)

The tree has three JSON readers that overlap: `fast-json` (the owner's library, a submodule, used by
the server's config and auth), the strict `Reader` inside `src/frontend/sessionsnapshot.h`, and
`src/frontend/jsonvalue.h` (new, for settings.json: strict, keeps member order, unknown keys and
the text of numbers, writes back). The owner dislikes implementing things twice and is willing to
extend `fast-json` if that is the right way. Reason first, then decide with the owner:

1. **Write the requirements as tests, once.** One test file with a small interface that each
   reader is adapted to (parse text -> ok/error, then read/write back), covering what we ask of a
   strict reader: syntax, duplicate keys refused, bad UTF-8 and lone surrogates refused, depth and
   size limits, order of members kept, unknown members kept on write-back, the text of numbers
   kept (1.0, 1e3, large integers), escapes, empty input, trailing garbage, NUL bytes. Run it
   against `jsonvalue.h`, the sessionsnapshot reader and `fast-json` (via a throw-away adapter;
   fast-json is a submodule: do not edit it in this step).
2. **Report which reader passes what** (a table in `docs/handover/features/agent2-json-readers.md`)
   and what it would take to make `fast-json` pass the rest (a change to the library, which the
   owner may do upstream, or a thin layer in this repo).
3. **Send the integrator the decision question** with 2-3 alternatives and a recommendation
   (A: `jsonvalue.h` is the one client-side reader and sessionsnapshot moves to it; B: everything on
   `fast-json`, extended; C: leave as is). Do not migrate anything before the owner decides.

Rules as before: tests first, small commits, tidy clean, SHAs to the integrator.

## Addendum (owner, 2026-11-23)
- **Writing JSON does not need to be centralised** (it is a simple function; sessionsnapshot and
  settings may keep their own writers). The question is about the *reader*: drop "write back" from
  the shared requirements, keep "unknown members kept" only as a property of the settings code.
- **Comments**: the owner would like his parser (`fast-json`) to accept comments in files people
  edit (`//` to the end of the line and `/* ... */`), as an opt-in mode (strict mode keeps refusing
  them, so protocol data stays strict). Add that to the requirements test (accepted in the comment
  mode, refused in strict mode, comment markers inside strings are text, an unterminated block
  comment is an error).
- **Speed is the point of fast-json and must stay**: add a benchmark (MB/s on a few shapes: many small
  objects, long strings, deep arrays, numbers) and run it for each reader; a comment mode must not slow
  the strict path (measure fast-json before and after in the same benchmark; the comment test must
  cost nothing when the mode is off). The reader is fast-json's, so an extension is a patch for the
  owner's library: prepare it as a patch file under `docs/handover/` (or a branch in a clone outside
  the submodule), do not commit inside `lib/fast-json`.
- **A question to bring to the owner with alternatives**: if settings.json may contain comments, the
  settings TUI must not lose them when it saves. (a) change only the value's bytes in the file (needs
  the parser to report the byte range of a value; keeps comments and layout; recommended),
  (b) rewrite the file and lose the comments (simplest), (c) keep the user's file read-only and write
  the TUI's choices to a second file. State what each costs.

## Addendum 2 (owner, 2026-11-23, replaces the comment parts above)
- Settings are handled mainly through the settings program, so **comments are not needed now: do not add a comment mode to fast-json at this time** (drop the comment cases and the byte-range question). Speed stays the principle of fast-json if it is touched later.
- **settings.json is rewritten whole, always formatted** (option b): the same layout every time it is saved, members in the order of the definitions, **every known setting written with its value, defaults included**, so the file shows the user what can be changed when it is edited by hand; unknown members are kept (after the known ones). Comments in an edited file are lost on the next save, which is accepted.
- The rest of this task (requirements test, table, decision between A/B/C for the reader) stands.
