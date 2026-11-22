#pragma once

// An overlay of a window (docs/TABS_PLAN.md section 8): a cell grid drawn over
// the panes, with its own border, that takes the input first. The window owner
// (WorkspaceView) places it, asks it to draw into a CellBuffer and forwards keys,
// clicks and the wheel; what comes back is what the user asked for.
#include "cellbuffer.h"
#include "cursorstate.h"
#include "prefixkeys.h"

#include <optional>
#include <string>

namespace frontend::ui {

struct CellSize {
    int columns = 0;
    int rows = 0;
};

struct LayerOutcome {
    bool close = false;              // remove the layer
    std::optional<CommandCall> call; // a layout command to run (after closing)
    std::string action;              // an application action (a CommandEntry id) without a call
};

class Layer {
public:
    virtual ~Layer() = default;
    // The size it would like in a window of this many cells (never larger).
    virtual CellSize wanted(int windowColumns, int windowRows) const = 0;
    // The size it really gets (what wanted() said, or less in a tiny window).
    virtual void resize(int columns, int rows) = 0;
    // Draws the whole grid; `cursor` gets the text cursor (visible = false: none).
    virtual void render(CellBuffer &cells, CursorState &cursor) const = 0;
    // A key; printable characters arrive as chords whose key is the code point
    // (Ctrl and Alt clear), named keys as keys::*.
    virtual LayerOutcome key(const KeyChord &chord) = 0;
    // A click at a cell of the layer's own grid.
    virtual LayerOutcome click(int column, int row) = 0;
    // The wheel; positive amounts scroll up (towards the start).
    virtual void wheel(double amount) = 0;
};

} // namespace frontend::ui
