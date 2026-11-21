#pragma once

// Shared vocabulary of the tabs/panes work (docs/TABS_PLAN.md). Plain data, no SDL,
// no terminals. The pane tree and workspace (workstream A), the renderer (B) and
// the tab strip (C) meet only here, so each can be built and tested alone.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frontend {

using PaneId = std::uint32_t; // 0 is "none"
using TabId = std::uint32_t;  // 0 is "none"

// A rectangle in drawable pixels, origin at the top left of the window area.
struct PixelRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PixelRect &o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

enum class SplitDirection {
    SideBySide, // children left and right, the divider is vertical
    Stacked,    // children above and below, the divider is horizontal
};

// Where one pane is drawn (output of PaneTree::layout).
struct PaneBox {
    PaneId pane = 0;
    PixelRect rect;          // the whole area of the pane in pixels
    int columns = 0;         // whole cells that fit; the program sees this size
    int rows = 0;
    PixelRect leftover[2]{}; // pixels right of and below the last whole cell
};

// A divider gutter between two panes (output of PaneTree::layout).
struct DividerBox {
    PixelRect rect; // the gutter strip
    SplitDirection direction = SplitDirection::SideBySide;
    PaneId before = 0; // left or upper neighbour (its scrollbar uses the near half)
    PaneId after = 0;  // right or lower neighbour
};

// One entry of the tab strip (input of the strip builder).
struct TabInfo {
    TabId id = 0;
    std::string title; // plain short text, already sanitized by the owner
    bool active = false;
    bool activity = false; // output or a bell in a hidden pane
};

// Everything the user can do to the layout. Key bindings, the prefix state machine
// and the mouse all produce these, so that tests need no window.
enum class Command {
    NewTab,
    CloseTab,
    NextTab,
    PreviousTab,
    SelectTab, // argument: tab index (0-based)
    SplitSideBySide,
    SplitStacked,
    ClosePane,
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    EvenOut, // snap every split to whole cells
};

} // namespace frontend
