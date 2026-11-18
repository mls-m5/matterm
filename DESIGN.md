# matterm Architecture and Roadmap

This document records the agreed direction, not a completed implementation.
The matterm-demo application starts with ASCII branding and a static gradient
welcome screen. User-triggered streaming and burst output exercise TerminalMemory,
RenderFrame, and SdlBackend. It draws numbered rows from a block-backed ring directly
through a borrowed cell view, without a separate visible-cell copy. Mouse wheel
and keyboard controls navigate history; a blinking cursor is shown at the live
bottom. ScrollViewport now provides smooth fractional-row navigation and scrollbar
proportions; SdlBackend clips shifted text and draws a right-hand scrollbar.
CellBuffer remains available as a CPU-only scratch surface. A separate
matterm-terminal-demo now uses TerminalModel for writing, cursor/erase operations,
primary history, and an isolated alternate screen. VT parsing, a PTY, and text
clusters are not implemented yet.

## Library and Application Boundaries

matterm is the project name. The build-tree target matterm::renderer is the common
rendering library interface. Keep it usable independently of terminal emulation.

- matterm-demo is a permanent, separate example application. Its welcome artwork,
  generated rows, and demo controls live under demo/, outside the renderer.
- matterm::terminal provides the CPU-only TerminalModel. matterm-terminal-demo
  exercises its direct API and renders through matterm::renderer. It has no shell.
- The future matterm terminal application will use these same libraries, adding
  a streaming UTF-8/VT parser, input encoding, and PTY ownership. The parser will
  call model operations rather than duplicating terminal behavior.
- A future standalone memory viewer can use a separate adapter to feed cells to
  the renderer without using the terminal emulator. Current CellGridView borrows
  same-process storage. Shared-memory/IPC support needs a defined layout, lifetime,
  and synchronization protocol; it is not implemented.

MATTERM_BUILD_DEMO controls both standalone demo builds. With BUILD_TESTING also
OFF, only the reusable libraries are built. No installable package is provided yet.

## Responsibilities and Ownership

- The GUI loop owns the rendering backend and keeps its window and context alive.
  Graphics resources must be destroyed before their context, on the right thread.
- TerminalModel handles screen state, text placement, cursor, erasure, and
  scrolling. Text placement uses cells, not pixels. A future separate parser
  decodes UTF-8/ANSI/VT streams into operations on this same model.
- Terminal memory owns cell rows, scrollback, and eventually the text cluster table.
- RenderBackend reads a frame description, draws it, and can present the image.
  Each concrete backend owns its atlases and other graphics resources.

CellBuffer provides draw(x, y, text) for direct matrix clients. TerminalModel
instead writes directly to its TerminalMemory screen rows and exposes a borrowed
RenderFrame, without a separate visible-cell copy. Neither API requires the other.

## Compact Cells and Text Clusters

FChar should have a small, fixed layout without pointers or per-cell allocations:

- CellText: a 32-bit value containing a code point or text cluster ID.
- Foreground and background colors, initially eight-bit RGB channels.
- Attributes, eventually including styles, wide characters, and continuation cells.

Reserve the highest bit of CellText as a cluster tag. Without the tag, the value
contains a valid Unicode scalar value; with the tag, the lower 31 bits contain an
ID in terminal memory's cluster table. The ID is neither a pointer nor a glyph
index. Encapsulate the representation behind named functions rather than bit
operations at call sites. Font changes or atlas rebuilds must not alter the text.

The target for these fields is roughly 12 bytes per cell. Verify the actual
layout with sizeof/static_assert when implementing it; do not use packed structs.

The first implementation only needs code points and simple ASCII rendering.
Prepare the representation for clusters, but implement cluster storage, shaping,
and full Unicode handling separately.

## Terminal Memory and Scrollback

TerminalMemory now uses a fixed-capacity ring of physical row slots backed by
blocks, with contiguous cells within each block. Blocks are allocated upfront;
rows and eventually whole blocks are overwritten in ring order without shifting
surviving cells. A partial final block keeps capacity exact. The configurable
block size defaults to 64 rows; the demo uses 16-row blocks and retains 256 rows
in total. These sizes are starting points, not measured optima.

A visible cell view should expose one row at a time. Each row is contiguous,
but different rows need not be adjacent. The backend interface must not require
a single contiguous vector for the entire screen. A view must also be able to
start inside scrollback and cross the ring buffer's wraparound point.

Current width resize clips or pads each retained row, preserving chronological
order and colors but not reflowing text. It explicitly reallocates and copies
retained cells; failure preserves the original storage. Height only changes the
visible slice in the direct-memory demo. TerminalModel additionally resizes its
screen: shrinking keeps bottom rows, growing appends blanks, and both buffers
resize together. Full text reflow remains open.

view(first, count) rebuilds only a bounded list of borrowed row descriptors,
clipping count to retained rows. Descriptors expire on the next view call,
mutation, or destruction; backends consume them synchronously. The demo tracks
scroll offset independently. Zero offset follows output; a historical offset is
advanced on append to keep the viewed rows anchored, then clamped when those
rows are evicted.

### Possible Future Cluster Reclamation

Terminal memory could use incremental mark-and-sweep instead of maintaining
reference counts on every cell modification:

1. Start a marking cycle with an epoch and track which live blocks remain to
   be visited.
2. Visit a bounded number of cells/blocks per work step and mark their clusters.
3. Reclaim unmarked clusters only after completing a full cycle.

Time since the last mark alone is not safe: a reference may live in a block that
has not yet been visited. Cell changes, copying, and new blocks during a cycle
need a write barrier that keeps newly introduced references alive. Newly allocated
clusters must also be protected. All other owning references must be included
among the roots or explicitly protected. Reused IDs must not leave stale
references behind. Sweep work should also be bounded, not just marking work.

This is a possible strategy, not a complete GC protocol. Start without clusters
or reclamation, and establish invariants and tests before implementing this.

## Frame Description and Backend Boundary

Collect the cell view and cursor in RenderFrame. CursorState describes cell
position, shape, and visibility; the cursor must not overwrite stored text.
Include the cursor early. Selection and other overlays can be added later.
Blink timing can live outside the backend.

Conceptual interface:

```cpp
class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual void render(const RenderFrame &frame) = 0;
    virtual void present() = 0;
};
```

Use one virtual render call per frame, not per cell. Cells and other views are
borrowed only during the render call and must not be retained by the backend.
When text clusters are introduced, the frame must also provide access to the
appropriate cluster lookup.

The backend handles cell pixel dimensions, drawable size, and HiDPI. SdlBackend
currently exposes cellWidth/cellHeight in drawable pixels and columns/rows for
the number of whole cells that fit. The shared metrics API remains a later
decision. RenderFrame now includes verticalOffsetPixels in (-cellHeight, 0],
textHeightPixels (-1 means full drawable), and normalized ScrollbarState.
Supply the partially visible extra row when the offset is fractional.
SdlBackend clips all text and cursor geometry to the text viewport, then draws
the scrollbar outside that clip. columns(true) reserves the 12-pixel sidebar.
The scrollbar currently indicates position only; it has a minimum thumb height
and does not handle clicking or dragging.

ScrollViewport keeps target and displayed offsets in fractional rows above the
live bottom. advance(seconds) applies exponential easing with a 70 ms time
constant and snaps near the destination. Mouse wheel preciseY values are retained.
When the target is historical, new output shifts both offsets to preserve content,
clamping at eviction boundaries. A zero target continues easing to the live bottom
without being pushed away again by each arriving row. The demo's
text viewport is a whole number of rows. Streaming output itself still advances
by whole rows; fractional animation currently applies to user navigation.

- A future OpenGlBackend will use direct OpenGL calls and SDL_GL_SwapWindow.
- SdlBackend uses SDL_Renderer and SDL_RenderPresent, owning its renderer and atlas.
  The window is borrowed from the GUI loop.
- These are alternative rendering paths, not mixed renderers in the same window.
  Construction and switching must respect each backend's graphics resource lifetime.

## Terminal Model and Primary/Alternate Screens

TerminalModel now owns screen switching, independently of rendering or a parser:

- Primary screen: visible cells plus bounded scrollback, with its own cursor
  and viewport state.
- Alternate screen: a separate screen-sized buffer without normal scrollback.
- Switching back restores primary cells, cursor, style, pending wrap, and viewport.
  The direct setAlternateScreen(true) API clears/homes the alternate screen on
  transition; repeated requests for the active mode are no-ops. The alternate
  buffer uses its own ring with capacity equal to screen height.
- This direct policy is not full DEC mode handling. Different VT sequences
  (including private mode 1049) require distinct save/restore and clearing rules
  in the future parser, with explicit tests.

RenderFrame stays mode-independent: submit the active cell view and cursor,
zero pixel offset for a fixed alternate screen, and optionally hide its scrollbar.
Do not conflate this mode with the keyboard's Alt modifier. Storage and direct
switching are implemented; VT parsing is not.

write() accepts UTF-32, handles CR/LF/BS/tab, defers wrapping at the last column,
and ignores unsupported controls. LF alone preserves the column. Erasure uses
current colors and never removes primary history. Characters currently occupy
one cell each; invalid scalars become U+FFFD. Resize keeps the insertion point
when widening a line whose wrap was pending. Screen sizes must be positive.

matterm-terminal-demo converts SDL text input to UTF-32 at its client boundary
using the shared incremental Utf8Decoder, not platform-dependent iconv formats.
The decoder buffers split characters, replaces malformed UTF-8, and flushes
truncated input on finish(). It can later be reused by the VT parser but does not
interpret terminal escape sequences.
F2 toggles screens, F3 generates lines, F4 erases the screen, and F5 resets the
model. Input controls demonstrate the API; they are not shell input encoding.
CPU tests cover model semantics and headless framebuffer tests compare primary
pixels and state before/after alternate rendering. Both demos remain permanent
library clients.

## Rendering and Performance

Start with SDL2's accelerated renderer and SDL_RenderGeometry (SDL >= 2.0.18).
Use a glyph atlas and batched indexed quads, for example one background pass and
one glyph pass, not one call per cell. Check which renderer backend SDL selects.
This path uses SDL_RenderPresent, not an application-owned OpenGL context.
A direct OpenGL backend with instanced quads remains a future alternative.
Atlas positions and glyph indices are backend details and must not live in FChar.

### Initial Glyph Atlas

The implementation is in src/glyphatlas.cpp, with its interface in
src/glyphatlas.h and per-glyph metrics in src/glyph.h. It is an SDL-specific
backend resource, not a shared terminal model abstraction. Implement an OpenGL
counterpart separately when needed; extract shared code only when its value is
clear from both implementations.
Use SDL2_ttf initially, with its 32-bit code point APIs (SDL2_ttf >= 2.0.18).
Rasterize on cache misses and upload into one fixed-size texture. Cache white
coverage glyphs and apply foreground colors through geometry vertex colors.
Store bearings and advance as well as atlas rectangles; place glyphs relative to
a shared baseline. Spaces can have metrics without visible bitmap pixels.
Gather glyphs before submitting geometry to avoid uploads interleaved with draws.

The first cache has no eviction: report a full atlas explicitly. This keeps
returned rectangles stable. Font size and face are fixed per atlas; changing
either requires a new atlas and rebuilding geometry. A fallback font or shaped
glyph cache will need a richer key than a Unicode code point.

Later, a bounded cache could track the last frame each entry was used. Do not
reuse a rectangle referenced by the current frame or queued rendering commands.
Collect and protect the visible working set before evicting entries, and rebuild
any geometry that refers to reused slots. Age is a replacement policy, not by
itself a lifetime guarantee. If the visible working set exceeds capacity, eviction
alone cannot solve the problem; more atlas pages or explicit failure is needed.
Glyph atlas eviction is independent of terminal text cluster reclamation.

Each render call draws a complete frame. After a buffer swap, do not assume that
the next back buffer contains the previous image. Partial GPU uploads are distinct
from partial redraws: later, only changed cells may need uploading while the
entire visible surface is still drawn.

Measure a static prompt, sustained scrolling, and full-screen updates. Track
CPU time, GPU time, uploaded bytes, and draw calls. Disable VSync for throughput
measurements. Add dirty tracking or further buffer optimizations only when
measurements justify them; no performance target has been verified yet.

## Next Steps

1. The row-based cell view, RenderFrame with cursor state, and RenderBackend are
   implemented. CellText tagging and cell attributes remain future work; no
   cluster allocation or GC yet.
2. The initial CellBuffer and its tests are implemented using test/mls-unit-test.
   It currently owns contiguous storage but exposes a row-based view compatible
   with future row blocks. Copy/move are disabled to keep borrowed-view ownership
   explicit. UTF-32 placement still assumes one code point per cell.
3. The SDL2_ttf atlas and cell buffer are connected through RenderFrame to
   SdlBackend. Drawing and presentation live in the backend, owned by the GUI
   loop. Font-dependent tests cover atlas behavior, cell colors, cursor shapes,
   cell updates, and empty frames. The interactive demo is temporary, not a
   terminal input or parsing implementation.
4. TerminalMemory's block-backed ring, eviction, width resize, and borrowed views
   are implemented and tested, including comparison with a reference queue across
   repeated wraps. The streaming demo exercises scrollback and bounded retention;
   backend tests render views crossing block and ring boundaries.
5. Smooth user scrolling, viewport anchoring, and a non-interactive scrollbar
   are implemented, with CPU and framebuffer tests. Optional scrollbar interaction
   and smooth live-output scrolling can be considered separately.
6. Branding and the static gradient welcome screen are implemented in the
   permanent matterm-demo client. S/Space toggles output, B bursts 300 rows, and
   R restores the welcome screen. Startup is idle, not a continuous stress test.
7. Basic terminal screen semantics and independent primary/alternate state are
   implemented in matterm::terminal and demonstrated by matterm-terminal-demo.
   Tests cover wrapping, controls, erasure, history, resize, and screen isolation.
8. Incremental UTF-8 decoding is implemented and tested independently, including
   split characters and invalid sequences. Next, design and implement a VT parser
   feeding the same model, with partial escape sequences and explicit DEC mode
   semantics.
   Add PTY/input encoding later; neither demo should be replaced. Measure,
   extend Unicode support, and optimize where justified.
