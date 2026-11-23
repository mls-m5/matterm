# Settings, Measurement, and Future Pipelines

## Settings Direction (Not Yet Implemented)

Prefer a versioned human-editable text file over a database for a single local
profile. Suggested path: `$XDG_CONFIG_HOME/matterm/config.toml`, falling back to
`$HOME/.config/matterm/config.toml`. Ignore relative XDG_CONFIG_HOME values.
No writable home/config directory must not prevent startup with defaults.

Use TOML only after selecting a parser/dependency; do not implement an ad-hoc
partial TOML parser. Keep a typed settings model separate from serialization,
SDL, and terminal protocol state. A future GUI edits that same model.

Proposed precedence: built-in defaults -> file -> command-line overrides.
Validate RGB values, palette indices, font sizes, and limits; report malformed
settings without silently overwriting the user's file. Missing files use defaults.
Explicit saves should create the directory, write a temporary file in the same
directory, flush/close, and atomically rename, with permissions appropriate for
user configuration. Define unknown-key/migration behavior before adding GUI writes.
Do not store passwords/session secrets in theme configuration.

Initial settings: font path/size, default foreground/background, 16-color palette,
optional 256-color overrides, scrollback limit, and blink/scroll preferences.
A dark built-in theme is a starting point, not a claim to match every GNOME version.

## What Completion Means

- Producer write completion: bytes were accepted by the tty transport, not
  necessarily parsed or drawn.
- A final cursor-position query and response is a portable-ish processing barrier:
  it establishes that the parser reached the query. It does not guarantee all
  pixels reached the display; some terminals have different scheduling behavior.
- CPU render submission/present return is an application/backend measurement,
  not proof of compositor presentation or physical scanout.
- GPU completion queries/fences can measure queued GPU work on suitable backends,
  still not physical display completion. There is no general terminal escape
  query that certifies the final image is visible on the user's screen.

Report these measurements separately. Use a timeout/raw input restoration around
DSR in a portable benchmark. Do not invent a private display-ack escape until
there is a real use case and a clearly defined semantic boundary.

## Benchmark First

Generate deterministic workloads before timing: ASCII long/short lines, scrolling,
SGR transitions, cursor-addressed full frames. No filesystem tree traversal or
random generation in measured sections. Internal runs feed the same bytes directly
to parser/model; portable runs write blocks to stdout. Consume/check observable
model results so optimization cannot remove the work. No per-cell profiler scopes.

Record bytes, wall time, repetitions/median, build mode/compiler, font/window grid,
backend, VSync, output chunk size, history limit, and tracing status. Separate warm
atlas/cache behavior from cold runs. Start with measurements, not optimizations.

## matperf Inspection

The matperf submodule lives at `lib/matperf/`. Its relocation from the repository
root was explicitly requested by the owner; the pinned commit was preserved
(c648bd1a1bc3390609ee79b046fed3d4c067c5b3). Do not modify or advance it implicitly.
README is minimal; the API is in include/matperf/profiler.h. It exposes
PROFILE_FUNCTION(), PROFILE_PART(literal), enableProfiling(settings), and thread
naming. Scope labels borrow string storage, so use static literals. Its public
header uses C++20 source_location; consumers need C++20 when tracing is enabled.

Current implementation writes Chrome trace JSON on a background thread, with a
10,000-entry per-thread ring. Overflow can throw or wait, changing measured
behavior. Before relying on traces, review synchronization: ring indices and the
writer running flag are plain fields accessed across threads, and readSpan marks
slots consumed before serialization. These are potential races/overwrite hazards,
not evidence that profiling results are already trustworthy. Review/fix upstream
separately rather than silently patching a user-owned submodule. Trace labels also
need JSON escaping for arbitrary strings. Binary serialization is future upstream
work, not part of this project change.

Plan an opt-in build/CLI switch, disabled by default. Instrument coarse boundaries:
session pump, parser feed, frame preparation, backend render, and present. Compare
baseline without tracing. Do not enable profiler threads before a forkpty launch.
No instrumentation inside hot per-byte/per-cell loops; preserve batching.

## Future Parallel Pipeline (Deferred)

Possible stages: transport -> ordered VT decoding/model updates -> immutable frame
preparation -> GUI-thread rendering, with bounded queues/rings between stages.
Do not parse arbitrary chunks concurrently: UTF-8, VT state, and model mutations
are ordered. Snapshots need explicit ownership/generations and backpressure; the
current borrowed row views cannot outlive concurrent mutation. SDL remains on its
required thread. Consider parallel work only after stage-level measurements show
benefit, and include latency/memory as well as throughput.
