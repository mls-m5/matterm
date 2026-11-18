# Terminal Architecture and Roadmap

This document records the agreed direction, not a completed implementation.
The application currently opens an SDL/OpenGL window and clears its background.
The headers are early sketches and will be adapted incrementally to this plan.

## Responsibilities and Ownership

- The GUI loop owns the rendering backend and keeps its window and context alive.
  Graphics resources must be destroyed before their context, on the right thread.
- The terminal model handles terminal state, text placement, cursor, scrolling,
  and eventually UTF-8/ANSI/VT parsing. Text placement uses cells, not pixels.
- Terminal memory owns cell rows, scrollback, and eventually the text cluster table.
- RenderBackend reads a frame description, draws it, and can present the image.
  Each concrete backend owns its atlases and other graphics resources.

A separate MatrixRenderer may initially provide draw(x, y, text) and a cell
matrix. It must not require a permanent copy of terminal memory. Eventually,
text placement can write directly to the terminal model, and rendering can read
its visible rows without an additional complete CPU copy.

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

Use a ring structure of row blocks, with contiguous cells within each block.
The block size is a later decision; 64 rows is an example starting point to
measure. Dropping old rows advances the start position and reuses retired blocks
rather than shifting all remaining cells. Bound scrollback to keep memory usage
predictable.

A visible cell view should expose one row at a time. Each row is contiguous,
but different rows need not be adjacent. The backend interface must not require
a single contiguous vector for the entire screen. A view must also be able to
start inside scrollback and cross the ring buffer's wraparound point.

Resize, line wrapping/reflow, and history details remain open questions.

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

The backend handles cell pixel dimensions, drawable size, and HiDPI. The
application needs a way to determine how many cells fit. The exact API is a later
decision. Sub-cell scrolling could eventually use a pixel offset; it is not
needed initially.

- OpenGlBackend uses direct OpenGL calls and SDL_GL_SwapWindow for presentation.
- A future SdlBackend uses SDL_Renderer and SDL_RenderPresent.
- These are alternative rendering paths, not mixed renderers in the same window.
  Construction and switching must respect each backend's graphics resource lifetime.

## Rendering and Performance

Start with OpenGL 3.3, a glyph atlas, and instanced quads. Aim for few draw calls,
for example one background pass and one glyph pass, not one call per cell.
Atlas positions and glyph indices are backend details and must not live in FChar.

Each render call draws a complete frame. After a buffer swap, do not assume that
the next back buffer contains the previous image. Partial GPU uploads are distinct
from partial redraws: later, only changed cells may need uploading while the
entire visible surface is still drawn.

Measure a static prompt, sustained scrolling, and full-screen updates. Track
CPU time, GPU time, uploaded bytes, and draw calls. Disable VSync for throughput
measurements. Add dirty tracking or further buffer optimizations only when
measurements justify them; no performance target has been verified yet.

## Next Steps

1. Adapt the header boundaries: CellText/FChar, a row-based cell view, RenderFrame
   with cursor state, and RenderBackend. No cluster allocation or GC yet.
2. Implement a small cell buffer with text placement and tests using
   test/mls-unit-test. Keep the storage boundary compatible with row blocks.
3. Draw a static ASCII grid and a cursor using the OpenGL backend and an atlas.
   Let the backend handle presentation too; the GUI loop owns it.
4. Implement terminal memory's row blocks/ring buffer and test wraparound,
   eviction of old rows, and visible views crossing block boundaries. Connect
   it to rendering.
5. Measure, then extend with terminal parsing, Unicode, and optimizations.
