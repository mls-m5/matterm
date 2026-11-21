# Server sessions: security and robustness review

An independent review of the persistent terminal sessions of `matterm-server`
(`src/server/sessionregistry.*`, `replayring.*`, the lease code in `auth.*`, the
attach/close paths in `server.cpp`) against the contract in
[PROTOCOL.md](PROTOCOL.md) section 11 and [SERVER.md](SERVER.md). Method: read
the code, write a threat model, write negative, model-based and fuzz tests, run
them. No production code was changed. One gap was found (S1, low); everything
else listed under "Holds" is covered by passing tests.

## 1. Threat model

Assets: the owner's shells and their output (the replay ring is a copy of what
the shell printed, possibly secrets), the host (every session is a process),
and the server's memory and CPU.

Who can do what: an **unauthenticated** peer reaches only `/login` (rate
limited) and gets 401 on `/ws` and the session actions, before any registry
lookup. The **owner** (one account) owns every terminal; a **stale cookie**
(rotated, logged out, expired) must lose all rights at once; a **second browser
tab or a stolen live cookie** is the owner for as long as the cookie lives, so
the registry's job is to keep the owner's terminals consistent and bounded, not
to tell tabs apart.

| Threat | Defence | Result |
| --- | --- | --- |
| Guessing or enumerating ids | 128 random bits, exactly 32 lowercase hex, generic `session unavailable`, auth first | **Holds** (3000 mutated ids, 25 over sockets: same reason, id never echoed) |
| A bad Hello evicts the live attachment | validation before any state change | **Holds** (live attachment never closed, no lease held after a refusal) |
| A refused attempt changes something | nothing may change | the PTY size changes before the lease check: **S1** |
| Old attachment acts after a takeover | attachment generation fence | **Holds** (input, resize, close, ack, detach, fail all ignored; also across three takeovers) |
| Old cookie acts after login rotation | `attachedValid` on every message and every 10 ms | **Holds** (20 inputs sent right after the rotation never reach the shell) |
| Expired cookie keeps a socket | same check; detached shells wait for the next login | **Holds** |
| Second Hello on an attached socket reaches another terminal | the session state machine | **Holds** |
| Lease leaks (denial of service by the owner's own tabs) | admit/release on every path, transfer on takeover | **Holds** (600 random operations, balanced to zero; never more than limit+1) |
| Replay memory growth | per-ring and total byte caps counting object overhead, wipe on drop | **Holds** (model test over 30 000 appends; 200 000 one-byte writes stay under the cap) |
| Stale or secret output after the end | wipe on exit, close-all, logout; tombstones keep status only | **Holds** (no output in a tombstone; close-all clears rings, tombstones and ids) |
| Unbounded detached shells | `maxSessions`, `maxDetachedSessions`, detached timeout from the detachment | **Holds** (rejected attempts do not extend the timeout) |
| Resume reads the wrong bytes | absolute offsets, contiguous ring | **Holds** (ring against a model: retained tail equals the last bytes written, safe start follows a newline) |

## 2. Finding

### S1. A refused attach still resizes the terminal (low)

`SessionRegistry::attach` toggles the PTY size (the one-cell SIGWINCH redraw
trick) while it checks that the attach can work, and only afterwards asks the
HTTP layer for the login lease (`transport.admit`). When the lease is refused
(`max_upgrades_per_login` reached) the Hello is answered with `upgrade limit`,
but the shell is already resized to the refused Hello's columns and rows. If
another tab is attached to that terminal it now shows a program drawn for the
wrong size until its next resize. The owner can do this to themselves (a stolen
cookie can do it to the owner) but it needs the lease limit to be full, so the
severity is low, and it contradicts "failed attempts never disturb an existing
attachment" (11.3).

Reproduce: `ctest -R sessions_issue_RefusedResize` (an expected failure).
Fix: take the lease first (as a takeover transfer when the previous attachment
is from the same login), then resize; if the resize throws, give the lease back
(`transport.release`). Moving the check keeps the intent ("preflight before
evicting the current attachment") because the old attachment is evicted only
after both steps.

## 3. Holds (and what shows it)

`test/sessionsreview_test.cpp`, suite `SessionsReview` (registry, no sockets) and
`SessionsReviewNet` (real server on loopback):

- The ring is checked against a model through 30 000 random appends of every
  size (including chunks larger than the budget): cost never above budget, the
  retained part is exactly the tail of what was written, reading in small pieces
  gives the same bytes, one byte before the oldest or past the end is refused,
  the safe start follows a newline.
- 3000 hostile Hellos (mutated and truncated ids, upper case, NUL, path text,
  offsets 0, 1, 4096, 2^31 and 2^64-1, zero and maximal sizes, odd versions and
  features, random refusal of the lease) never close the live attachment, never
  hold a lease after a refusal, only produce the fixed error texts, and leave
  the parked session resumable.
- Late events of a replaced attachment (input, resize, close, ack, detach,
  fail) do nothing, also after a third takeover.
- The detached timeout counts from the detachment; refused attempts (bad
  offset, refused lease) do not extend it.
- Exit tombstones are bounded by `max_sessions`, hold no output, report the
  exit status, and disappear with close-all, after which every old id is refused
  and the registry works again.
- Close-all closes every attachment, kills every shell, empties the rings and
  leaves no lease behind.
- 600 random operations (attach, takeover by id, detach, fail, close-all, tick)
  keep the leases consistent: a connection holds a lease only while attached,
  the total never passes the limit by more than the one transferred slot, and it
  is zero at the end.
- Over sockets: input from a socket whose login was rotated is never delivered;
  an expired cookie closes the socket and the shell waits for the next login;
  garbage Hellos from a second socket leave the owner untouched.

Already covered before this review (`serversessions_test`, `serverregistry_test`,
`serverreplay_test`): login rotation, logout and close-all with their Origin and
method checks, concurrent upgrade limits, resume with replay.

## 4. Observations (no change needed, or a note)

- Replay is a raw byte copy. It is wiped when dropped (`sodium_memzero`), but
  the copies in output messages and in socket buffers are not; a terminal
  program that prints a secret leaves it in the ring for `detached_timeout`
  (default one hour). That is the accepted cost of resume (PROTOCOL.md 11.4); the
  operator should keep `replay_buffer_bytes` small and use close-all.
- Detached shells survive cookie expiry and login rotation by design; only the
  password gets them back.
- `invalid resume offset` (offset past the end of an existing terminal) is a
  different answer from `session unavailable` for an unknown id. It can only be
  seen by someone who already holds a valid id and the owner's cookie, so it
  reveals nothing new.
- Any exception from `tick()` closes every session (a fail-safe, but a single
  bug in one ring would end all of them).
- A tombstone can be attached any number of times until it is evicted or
  close-all runs; it holds a lease only until the socket closes at once.

## 5. Tests

| File | What | Registered as |
| --- | --- | --- |
| `test/sessionsreview_test.cpp` | ring model, hostile Hellos, fences, timeouts, tombstones, close-all, lease model, socket checks | `sessionsreview` |
| `test/sessionsissues_test.cpp` | S1 | `sessions_issue_RefusedResize`, `WILL_FAIL`, label `known-issue` |

A `WILL_FAIL` test turns red when the code is fixed; then move the suite into
`sessionsreview_test.cpp` and delete its `sessions_issue_` entry.
