# matmux list metadata: backend implementation

## API checkpoint (preceding the cache commit)

Control format/breaking remain 1/3; additive is now 5. Existing list/info and
frozen handshake bytes are unchanged. GetSessionDetails (0x4c) takes 1..16
unique lowercase-hex session IDs and includePreview (false by default).
SessionDetails (0xcb) carries processedCount and rows. processedCount includes
vanished IDs; rows stop before the unchanged 16384-byte frame cap. The caller
may defer the unprocessed suffix, must not treat it as vanished or loop without
progress, and must validate returned IDs against its consumed request prefix.

SessionDetail fields: id, name, cwd, command, optional foregroundBusy,
optional outputAgeMs, optional preview. Unknown/false foreground are distinct;
shell foreground is NOT an idle-prompt claim. Preview null and present empty
are distinct. Its contract is decoded output, not last command, rendered row or
secrecy filter. The codec validates <=1024 UTF-8 bytes / 256 scalars, single-line
C0/C1-free and no bidi controls. Shared mtd::validPreviewText/validPreviewScalar
are available to the frontend and collector. Command argv uses <=4096 aggregate
bytes including terminators, <=32 args / <=1024 bytes each. Names/cwd retain
legacy bounds. Unknown flags, illegal busy-without-known, duplicates, malformed
UTF-8, truncated/trailing data and oversized frames are refused.

The metadata handler is owner-control-only; exposed connections are denied.
It does not attach, resize, acknowledge or drain a PTY. Legacy /proc busy
heuristics for close prompts stay unchanged; PaneState has a separate optional
foregroundBusy, populated only when a foreground PID and argv are known.
Bad/unreadable process metadata does not manufacture shell/idle status.

Registry::outputState/readOutput are const retained-ring readers. ReplayRing
newestTime uses the already-recorded last Chunk timestamp: no new output-path
clock/update. No retained chunk => unknown age. Age is since daemon READ,
not process production; replay reads do not refresh it. Offset reads locate a
chunk with logarithmic partition_point before copying a bounded range instead
of walking the whole deque. readOutput caps copies at 16 KiB.

Only the codec's small existing UTF-8 decoder is added to daemon-wire/server
link dependencies; no terminal model/VT dependency is introduced in web-server
core. The decoder/cache belongs to daemon-core only.

At this intermediate API checkpoint previews remain unavailable even if asked
for; no output is decoded or transmitted. The separately tested daemon-only
cache is the next backend stage. Clients must gate details on version >=1.3.5
and permanently fall back for an unsupported request to a legacy daemon, with
unknown age/preview rather than attaching.

## Verification at API checkpoint

Tests-first codec and metadata/replay cases failed compilation before their
APIs existed. session_details covers optionals, all truncations, duplicate/
malformed IDs, flags, Unicode/control/bidi/scalar limits, aggregate command and
16-KiB bounds. daemon_metadata uses a real daemon: detached output discovery,
no attachment/takeover, existing attached-client Ping/Pong preserved, web denial,
old-daemon refusal and vanished IDs. serverreplay covers timestamp semantics and
all offset boundaries through thousands of small chunks. New cache/expiry/off
and daemon flood measurements were not yet implemented at this API checkpoint.

## Daemon-only preview cache

The cache is now wired into opted-in owner metadata requests, never PTY writes.
It uses the reviewed model-free VtParser, and two fixed 256-scalar arrays per
entry (<=2 KiB text, no growing raw history). Retained scalar arrays are wiped
when cleared/expired/evicted; this is not a guarantee about temporary codec/IPC
copies. Unsafe Unicode/bidi marks a whole fragment unavailable rather than
stripping a control and presenting potentially misleading payload text.

There are at most 16 entries, explicit request-order LRU independent of timestamp
resolution, 16 KiB input per ID/request and a daemon-wide 64 KiB fixed one-second
decode window shared across all connections. This is a fixed-window work budget,
not a rolling-window bandwidth promise; adjacent windows can have boundary bursts.
Off cannot refill the budget. A prefix retained from stream offset zero can be
processed incrementally up to 32 KiB backlog. Larger backlogs destroy all old
fragments, lexer and UTF-8 state and restart at the most recent 16 KiB,
unsynchronized even if offset zero remains retained. An evicted/cold suffix is unknown until a genuine
ST/CAN/SUB. Unprocessed offsets never return old cached fragments as fresh.
Age and foreground classification remain independent of preview availability.

An Asio timer is armed only while cache entries exist and expires inactive
entries after five seconds even if no further RPC arrives. Refresh updates the
next deadline; off clears every requested ID, including an unprocessed reply
suffix. EndSession removes its entry; daemon shutdown and metadata exceptions
clear sensitive fragments and cancel expiry. No new thread or idle poll loop.

terminalListPreview is a client bool setting, default true, handled by the
existing canonical settings model and the TUI Terminal section. Client adapter
wiring is agent3's; includePreview=false performs no raw read/decoding and
returns no text. The setting is not a global replay-retention policy or an ACL
against another owner client requesting previews.

Seven cache cases pass, including same-millisecond LRU, empty/unknown distinction,
CJK/four-byte scalars at exactly 1024 bytes, unsafe strings/bidi, cold and
unprocessed tails, off/no-read, global work budget and expiry deadlines. An
independent read-only review found the original timestamp-only LRU tie bug;
the new regression was red and fixed with an explicit recency list. Real daemon
coverage now includes opt-in/off output, multi-connection budget sharing and
16-KiB prefix pagination with long folders. The follow-up source review at
23302ed found no issues; the backlog change received parent source inspection
and red-first OSC/DCS regression f0a5129. Attempting to resume the independent
reviewer for that final change failed twice with `Async run not found`; no
alternate review route was launched. Independent final-backlog review remains
an explicitly uncompleted verification item.

## Reproducible flood measurements

Build `matterm-metadata-bench` with MATTERM_BUILD_BENCHMARKS=ON; run:

```
matterm-metadata-bench /absolute/path/matterm-daemon on 5 4 text
# Modes none/off/on; workloads text/string/rep/ansi; sessions 1..16.
```

Each run owns a separate detached daemon, four paced Python producers, one
second startup warmup, and metadata polling at most once per second. It never
attaches and prints only counters, not output. `pty_MiB_s` is aggregate retained
output-offset growth during the measured interval. SELF CPU is taken from the
existing daemon perf-log final summary, includes startup/warmup, and excludes
child-program CPU. Python3 is required. Shutdown is bounded to five seconds
before killing only this benchmark's child. Temporary sockets/logs are removed.

Matched Release GCC -O3 -DNDEBUG, static, RELRO/NOW/noexecstack: baseline daemon.cpp
from API checkpoint f122045 versus 608a417, linked against identical dependencies
and flags. The baseline has the additive metadata API but no preview cache.
Five-second runs (six-second CPU interval) on this shared container:

| Daemon | Mode | Aggregate MiB/s | SELF CPU ms | CPU % | Known/queried |
|---|---|---:|---:|---:|---:|
| API baseline | no list | 1.540 | 74.2 | 1.2 | — |
| API baseline | off | 1.540 | 73.7 | 1.2 | 0/20 |
| API baseline | on | 1.537 | 73.1 | 1.2 | 0/20 |
| cache | no list | 1.540 | 71.1 | 1.2 | — |
| cache | off | 1.537 | 76.6 | 1.3 | 0/20 |
| cache | on | 1.543 | 61.6 | 1.0 | 20/20 |

A second three-second set gave 1.536–1.552 MiB/s and 0.6–1.2% CPU across
baseline/cache/modes. These short, paced, shared-host measurements show no
observable throughput regression, NOT a CPU speedup or saturated throughput
claim; CPU noise is larger than the preview work. The benchmark loop was
interrupted after the first full six-run set, then restarted for the second.
No incomplete row was used in the table.

The first real flood exposed a freshness failure without a security leak:
before backlog restart, only 4/48 previews were known over 12 seconds because
16 KiB/s incremental decoding cannot catch ~400 KiB/s producers. After the
bounded unsynchronized restart, 48/48 were known at essentially unchanged
1.54 MiB/s and 1.3% SELF CPU. This workload deliberately includes genuine ST.

Additional three-second cache runs, four sessions, off/on pairs:

| Workload | MiB/s off/on | CPU ms off/on | Known on |
|---|---|---|---|
| 4 KiB OSC payload + ST | 1.531 / 1.541 | 49.3 / 48.8 | 12/12 |
| huge REP count (no model expansion) | 1.541 / 1.552 | 45.3 / 33.5 | 12/12 |
| ANSI/CRLF only, no ST/CAN/SUB | 1.546 / 1.536 | 38.9 / 45.4 | 0/12 |

ANSI-only cold tails intentionally stay unknown; LF, BEL and CSI are NOT safe
synchronization boundaries. Sixteen sessions reached 6.019 MiB/s, 153.9 ms SELF
CPU (3.8% over four seconds), with 12/48 known rows: only four 16-KiB tail scans
fit each global 64-KiB window. Budget exhaustion and no safe boundary remain
indistinguishable unknowns in the public API, not misleading stale previews.

## Final verification

Warning-free full build; initial 145/145 tests passed before latest frontend
merge. Final merged run passed 144/145: the older matmux PTY helper mistook a
list preview for successful terminal opening. With agent3's test-only e867446
(imported as 3b03ecb), the real `matmux_pty` passes, including decoded preview,
OSC payload exclusion and setting off/on reload. Final combined rerun: **145/145
passed**, 119.51 seconds with four workers, warning-free full build.
Only the two previously unsafe cleanup `layout_smoke_(local|remote)` tests are
excluded. Focused clang-tidy on daemon/cache/benchmark emits no user diagnostics.
No parser changes were made after its separate reviewed/fuzzed checkpoint.
