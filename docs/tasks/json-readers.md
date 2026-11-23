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
