# Stage 1: model-free text mode of the existing VT parser

Scope: approved parser stage of docs/tasks/matmux-rich-list.md and the metadata
proposal. No daemon protocol, replay accessor, metadata cache, adapter or settings
flag is implemented by this stage. The integrator reviews this parser stage
before metadata implementation.

## API

`VtParser(VtParser::TextOnly{sink, startAtKnownBoundary})` constructs no terminal
model. `sink` receives temporary `u32string_view`s containing decoded ground text
or CR/LF/HT boundaries; it must copy into its own bounded collector and must not
re-enter the parser. `textSynchronized()` is false for normal model parsers.

Use startAtKnownBoundary=true only at offset zero or a proven ground boundary.
Cold/evicted tails use false: discard until ST (ESC backslash / UTF-8 C1 ST) or
CAN/SUB. LF, BEL and nested string openers do not synchronize a cold tail.
`resetInput()` in text mode loses synchronization; `finish()` flushes pending
UTF-8 only at known ground and does not unlock an unfinished control payload.
Invalid UTF-8 fails closed; literal U+FFFD conservatively does so too. Raw 8-bit
C1 therefore makes the preview unavailable until a valid synchronization marker.
UTF-8 encoded C1 control introducers/terminators are mapped into the existing VT
machine only in text mode. Normal model-mode C1 semantics are unchanged.

This is recent decoded output, not a rendered screen or last command. No cursor,
SGR, REP, scroll, alternate-screen, title, reply or palette semantics execute.
In particular this is not a secrecy/visibility filter for printable characters
under rendition modes such as SGR conceal. The metadata/UI must retain the
sensitive-preview disclaimer and disable option from the approved contract.

## Reuse and bounds

There is ONE ParserState<TextMode> implementation with the existing UTF-8
Decoder and byte/escape/CSI/string state machine. Compile-time specialization
omits text-mode dispatch checks in the normal per-byte path; selecting an opaque
implementation costs one branch per feed chunk. Only the selected implementation
is allocated, never two, and the text implementation allocates no TerminalModel.

Text mode never accumulates OSC/DCS payload or performs semantic amplification
such as CSI REP. Input decoding is byte-incremental through the same Utf8Decoder,
so CAN/reset cannot clear UTF-8 belonging to a later position in the same batch.
Retained state is fixed parser/decoder fields and no growing control-string
payload. Temporary decoded units are bounded by one input byte plus an invalid
pending sequence. The sink owns its output bound; the API does not accumulate
unbounded extracted text itself.

## Tests and independent review

- Tests-first 1181f6c failed compilation before TextOnly existed.
- Cold LF/BEL, OSC/DCS/APC/PM, UTF-8/CJK/C1, invalid UTF-8, huge strings/CSI REP,
  every split, one-byte feeds and deterministic arbitrary input are covered.
- Independent read-only review found ESC + ignored C1 + backslash could create
  a false ST. The regression was red before the fix and now covers every
  unhandled C1 in cold/known OSC/DCS, every split and one-byte feeds. Ignored C1
  consumes the existing discard transition instead of preserving StringEscape.
- Follow-up independent review found no remaining issue, including headless
  null-model paths and the compile-time specialization. Review did not itself
  execute tests/benchmarks; the parent ran those below.
- Parser unit binary: 20 cases pass; vt_fuzz passes. Full native warning-free
  build; ctest -j4: 142/142 passed in 88.22 s, including matmux_pty, native log,
  settings UI/PTY/native and soak. Only existing layout_smoke_local/remote were
  excluded because of pkill -f cleanup. Focused clang-tidy is clean.

## Local Release measurements

Same existing parserbench workload/driver, model 80x24, history 1000, 8 MiB,
4096-byte chunks, warmup + nine repetitions. Three alternating before/after
processes; table gives the median of their medians. Before source was exported
from pre-implementation HEAD to build/textmode-release/baseline. Both parser
objects used IDENTICAL -O3/-DNDEBUG/-fPIC, FORTIFY=2, _GLIBCXX_ASSERTIONS,
stack-protector and overflow/null-pointer flags; the same model/decoder/palette
libraries and benchmark driver were used. All output checksums matched.

| Model workload | Before ms | After ms |
|---|---:|---:|
| long | 73.447 | 73.087 |
| short | 80.146 | 79.955 |
| color | 59.519 | 57.185 |
| redraw | 66.668 | 66.251 |

The initial runtime-mode prototype showed a color-heavy regression; it was
replaced by compile-time specialization. Final measurements show no consistent
normal-mode regression in these samples, not a promise about every program.

A bounded count/hash sink, 8 MiB, 4096-byte feeds, warmup + seven repetitions,
Release/hardening, gave text-only 148/146/144/149 MiB/s for long/short/color/redraw,
167 MiB/s for an OSC payload (zero emitted characters), and 151 MiB/s for repeated
CSI REP (one initial character per run, no expansion). Thus the proposed global
64 KiB/s decode budget corresponds to <0.5 ms/s core decoding in this local test;
future ring lookup, collector, /proc, codec and IPC costs are NOT included.
The temporary measurement driver/source and raw logs are under this workspace's
build/textmode-release and /tmp/agent2-text-final-*-bench.log; normal measurements
use the existing matterm-parser-bench target.

No real display/SSH/LAN measurement and no daemon preview-on/off flood comparison
has been performed at this parser-only stage.
