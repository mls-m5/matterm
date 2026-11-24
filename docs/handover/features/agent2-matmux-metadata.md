# matmux list metadata: backend implementation

## API checkpoint (preview cache follows separately)

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
core. The upcoming decoder/cache belongs to daemon-core only.

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
and daemon flood measurements are not yet implemented at this checkpoint.
