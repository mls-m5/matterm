# Proposal: bounded metadata for the three-line terminal list

Design only; no daemon protocol or parser implementation is included. Inspected
against main 3ae5b94; merged into agent2 as df015d7. Agent3 owns ttylist.h and
its frontend/PTY tests. Shared protocol/source/settings ownership needs approval.

## Findings in the existing code

- `Entry::pump` in src/server/sessionregistry.cpp already appends fresh PTY
  bytes to ReplayRing with the pump's monotonic timestamp. Replay does not call
  append again. No attach/takeover is needed to read this storage.
- ReplayRing already bounds retained bytes per session (daemon default 4 MiB)
  and globally (256 MiB). Its last Chunk timestamp can provide output age in
  O(1), without another clock or per-output update. If no retained chunk exists,
  age is unknown. It means time since the daemon READ retained output, not
  time since the program generated it (backpressure can delay reads).
- ReplayRing::read currently walks chunks from the front. A small byte limit
  alone does NOT bound the work of finding a tail offset. Use a reverse-tail
  accessor or lower_bound on chunk end offsets before claiming cheap tail reads.
- ReplayRing::safeStart is the first LF after eviction. It is a replay heuristic,
  NOT proof that a suffix begins outside OSC/DCS/APC/PM or a split UTF-8 sequence.
  Newlines can occur inside string controls. Starting a parser there could expose
  hidden OSC payload as ordinary text. Never treat it as a safe preview anchor.
- VtParser is the only existing output VT parser. KeyDecoder parses input, not
  output. Chat sanitization has a chat-specific character allowlist and would
  replace CJK, so is not an appropriate preview decoder.
- VtParser currently requires TerminalModel. Its semantic CSI operations include
  REP with up to 65535 repeated characters per sequence. A small shared scratch
  model plus a byte cap therefore does not guarantee inexpensive parsing of
  hostile or unusual output. Dropping raw ESC bytes also leaves OSC payloads.
- Existing GetSessionInfo uses /proc without attachment. Its bool busy conflates
  some unknown states with shell/non-shell fallbacks. A new optional foreground
  classification must not turn an unreadable foreground into 'idle prompt'.

## Alternatives

A. **Age + existing process/attachment information first, preview unavailable.**
   Minimal risk; newestTime() is a trivial ReplayRing accessor. Fully compatible
   with the frontend's third-row fallback. Does not deliver useful previews yet.

B. **Recommended full preview: request-time replay decoding, reusing VtParser
   in a model-free text-only mode.** Add a text-only constructor/sink to the
   existing parser, using its SAME incremental UTF-8/control state machine.
   Suppress model actions, replies, titles, palette callbacks, and OSC payload
   accumulation; CSI REP/cursor operations must not do semantic work. This is
   a bounded mode of the existing parser, not a second VT parser or a model per
   entry. Parser work happens only for requested previews, never on PTY writes.

   Cache parser continuation, decoded offset and bounded current/previous text
   fragments for at most 16 session IDs, LRU/5-second inactivity expiry. Expiry
   must actually clear inactive entries (a timer only while cache is nonempty),
   not merely wait for a later request. Each entry stores at most two fragments
   of 256 Unicode scalars (<=2 KiB text total) plus bounded parser state. OSC/DCS
   payload is never cached. No preview text goes into logs.

   Start in normal ground state only at output offset 0 or a retained cached
   continuation. For a cold/evicted tail, start conservatively unsynchronized:
   reuse the parser's discard-string state and accept synchronization only at
   an unambiguous ST (`ESC \\`) or CAN/SUB, not LF or BEL alone. Until then return
   unavailable. A common ANSI-only old shell may consequently have no preview;
   this is preferable to exposing a partial hidden string. No guarantee of a
   preview for every session is proposed.

   Normalize decoded text to a control-free single-line recent fragment; CR/LF
   delimit fragments, HT separates words. Return the newest nonempty fragment,
   not a command or reconstructed visible screen row. Retain all valid printable
   Unicode including CJK; frontend still sanitizes and clips by display width.
   Return a preview only when parsing is synchronized AND caught up to the
   sampled outputEnd. A partially processed backlog stays unavailable, not
   falsely fresh. Subsequent refreshes can continue within the retained ring.

C. **Always parse/store a preview as output arrives.** Predictable availability
   from session birth, but adds a scan to all output even with no list open.
   Requires daemon collection policy separate from a remote client's display
   setting. Not recommended under the requested disabled-performance constraint.

B requires explicit approval to extend src/terminal/vt/vtparser.{h,cpp} and its
tests before backend protocol work. If that refactor is not wanted now, choose A;
do not improvise a separate escape stripper or unsafe raw-tail preview.

## Proposed metadata / compatibility contract

Keep ListSessions/SessionList and GetSessionInfo/SessionInfo byte-for-byte.
Add control 1.3.5 (breaking stays 3):

- `GetSessionDetails { vector<string> ids; bool includePreview=false; }`, type
  0x4c, at most 16 IDs, duplicates/malformed IDs rejected.
- `SessionDetails { uint16_t processedCount; vector<SessionDetail> rows; }`, type
  0xcb. Process IDs in request order; processedCount includes vanished IDs. If
  the next row would exceed the frame cap, stop before it. The client can defer
  unprocessed IDs to the next bounded refresh rather than assume they vanished.
- `SessionDetail`: id, name, cwd, command (same bounds/semantics as SessionInfo),
  `optional<bool> foregroundBusy`, `optional<uint64_t> outputAgeMs`,
  `optional<string> preview`. Preview is <=1024 UTF-8 bytes / 256 scalars.
  Null means unavailable; an empty present string means known empty. No boolean
  'active' combines these independent signals. Attachment stays in SessionEntry.
- Encode presence explicitly (known bits); reject unknown bits, invalid UTF-8,
  out-of-range payloads and trailing bytes. IMPORTANT: the existing daemon
  FRAME cap is 16384 bytes, not the 512-KiB outgoing queue limit. Retain it.
  Enforce <=4096 aggregate command bytes including argument terminators (the
  existing process probe already uses that cap), <=4096 cwd, <=64 name, <=1024
  preview and a 32-byte ID. One maximal row is <10 KiB and always fits. Build
  each response to <=16384 bytes, returning only the prefix that fits, with
  processedCount for continuation. Disappeared processed IDs are omitted and
  mapped to unknown by the caller, not fatal to the whole request.
- `foregroundBusy=false` only after identifying the foreground process as a
  shell; true after identifying a non-shell program; unreadable foreground =>
  null. Shell foreground does NOT establish an idle prompt or last command.
- Old daemon (additive <5 or unknown): don't send the request. Keep existing
  bounded list/info behavior, unknown age/preview. Legacy bool busy should not
  manufacture a known foreground state from an ambiguous fallback. Unexpected
  BadMessage/closed unsupported request disables details for that daemon cache.
- Old clients never request the extension and retain old replies. ExposedHello
  cannot use the request: private socket-owner control clients only, consistent
  with GetSessionInfo. No attaching, acknowledgements, resizing or PTY draining
  are caused by a list/details request.

One batch replaces per-row info round trips for new daemons. Frontend requests
visible IDs, at most once per second, caching other rows; add local elapsed time
to an age snapshot if displayed between polls. Backend has no 'recent' threshold:
that is presentation policy, not process state. No polling-driven busy redraw.

## Cost bounds and preview-off behavior

Proposed limits: <=16 KiB source bytes decoded per ID per refresh, <=64 KiB
TOTAL decoded per second across the daemon (not per client), <=16 cached IDs,
<=2 KiB retained decoded text per cached ID, plus bounded existing lexer state.
Use the ring's already-recorded timestamps; no additional clocks or per-byte
work on ordinary PTY output. Preview off means no decoding, no raw text in a
reply, and clearing requested IDs from the preview cache. Age/process metadata
remain usable. CPU time/allocator overhead still needs measurement; these are
byte/memory policy bounds, not measured latency promises. Benchmark a flood
with no list, previews off, and previews on, plus CSI REP/string-heavy inputs.

## Settings proposal (approval pending)

`terminalListPreview`: bool, proposed default true (feature requested; owner can
choose false if privacy-first). Existing settings definitions/UI, Terminal
section; no independent config file or new writer. Applies to the CLIENT's file,
also with --daemon-host, and changes includePreview on list refresh. Frontend
clears existing preview display when disabled and keeps all three-row selection/
navigation improvements. Agent3 has explicitly left this definition/backend
flag to agent2 after integrator approval; he owns no competing setting.

This setting prevents preview extraction/transmission/display, not existing
replay history (which already exists for attachment). It is not shell no-echo
policy and cannot promise that previously printed secrets were never retained.

## Tests before implementation

Deterministic fake-clock newest-output ages, unknown/no-output/evicted chunks;
no replay double-age update; bounds and efficient tail lookup; the same VT/UTF-8
split-point tests in model and text-only modes; OSC/DCS spanning chunks, LF
inside strings, cold tails starting inside a payload, CAN/ST synchronization;
CSI REP work stays bounded; invalid/truncated UTF-8; strict codec caps and old
version fallback; private/exposed access; no attachment/generation changes;
preview-off causes zero decoder calls and no returned text; caches/expiry;
client setting keeps unknown members and canonical saves. Existing daemon,
parser, matmux end/startup and performance regressions remain required.
