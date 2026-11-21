#pragma once

// SDL-free glue between a Workspace and a window (native app and browser): turns
// workspace state + window size + cell size into a frame plan, and routes input
// (keys, mouse, wheel) to layout commands or to a pane. No SDL, no drawing, no
// sending of bytes: the owner draws the plan and encodes/sends what is forwarded.
//
// Pane requirements: `TerminalView &view()` (and const). The glue calls
// view().resize(columns, rows) itself when a pane's cell size changes (sync()).
//
// Typical use per frame / event:
//   WorkspaceView<Pane> wv(workspace);
//   wv.setWindow({drawableW, drawableH, cellW, cellH});   // on start and resize
//   key:    KeyResult r = wv.key(chord, nowMs);   -> r.copy/paste/forward*, r.empty
//   mouse:  MouseResult m = wv.mouse(ev);          -> m.pane (forward to that pane)
//   wheel:  WheelResult w = wv.wheel(x, y, amount) -> w.pane (forward) or consumed
//   frame:  wv.tick(nowMs); wv.sync(nowMs); FramePlan p = wv.plan();
//           then draw p.strip, each p.panes[i] (view frame, rect, scrollbar), p.dividers.
// Splits keep their ratios when the window changes size.
#include "cellhit.h"
#include "palette.h"
#include "panetree.h"
#include "prefixkeys.h"
#include "tabstrip.h"
#include "tabtypes.h"
#include "terminalview.h"
#include "workspace.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frontend {

struct WindowMetrics {
    int width = 0; // drawable pixels
    int height = 0;
    int cellWidth = 8;
    int cellHeight = 16;
};

struct ViewConfig {
    int dividerThickness = 12;   // gutter strip in pixels (its near half holds a scrollbar)
    int scrollbarStrip = 12;     // reserved at the right edge of panes touching it
    bool showStripWithOneTab = true;
    bool prefixKeys = true;      // Ctrl+B ... (browser)
    bool desktopShortcuts = true; // Ctrl+Shift+T ... (native)
    PrefixKeys::Config prefix;
    std::int64_t resizeIntervalMs = 40; // at most one size change per pane this often during a drag
    Palette palette;             // for the strip
};

// One pane as it should be drawn.
struct PanePlan {
    PaneId id = 0;
    PixelRect rect;      // whole area; draw with PaneArea{rect, scrollbar}
    PixelRect scrollbar; // gutter half next to the pane; empty (w == 0): the strip inside rect
    int columns = 0;     // size the program sees (view().model() has this size after sync())
    int rows = 0;
    bool focused = false;
};

struct FramePlan {
    PixelRect stripRect; // empty (h == 0) when the strip is hidden
    TabStrip strip;      // cells of the strip row, columns = stripRect.w / cellWidth
    std::vector<PanePlan> panes;
    std::vector<DividerBox> dividers;
    PaneId focus = 0;
};

// Mouse input in drawable pixels.
struct MouseInput {
    enum class Type { Down, Up, Move };
    enum class Button { None, Left, Middle, Right };
    Type type = Type::Move;
    Button button = Button::None;
    int x = 0;
    int y = 0;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    int clicks = 1;
};

// A mouse event for a pane: position relative to the pane's rect, plus the cell.
struct PaneMouse {
    PaneId pane = 0;
    double x = 0, y = 0;                       // relative to the pane's rect origin
    std::optional<TerminalModel::TextCell> cell; // under the pointer, none outside the text
    TerminalModel::TextCell clamped{};         // nearest cell (for drags that left the text)
    bool focusedByThis = false;                // this press moved the focus here
};

struct MouseResult {
    bool consumed = false;               // chrome (strip, divider, scrollbar) took it
    bool changed = false;                // layout or view changed: redraw
    std::optional<PaneMouse> pane;       // forward to this pane (selection, mouse reporting)
    enum class Cursor { Default, ResizeColumns, ResizeRows } cursor = Cursor::Default;
    bool empty = false;                  // the last tab was closed
};

struct WheelResult {
    bool consumed = false;               // the strip took it (tab switch)
    std::optional<PaneId> pane;          // forward to this pane (under the pointer)
    bool changed = false;
};

struct KeyResult {
    std::vector<CommandCall> commands;   // layout commands that were applied
    bool copy = false;                   // Ctrl+Shift+C: copy the focused pane's selection
    bool paste = false;                  // Ctrl+Shift+V
    bool forwardPrefix = false;          // send the prefix key to the focused program first
    bool forwardKey = false;             // then this key
    bool changed = false;                // layout changed: redraw
    bool empty = false;                  // the last tab was closed
};

struct FocusChange {
    PaneId lost = 0;   // 0: none
    PaneId gained = 0;
};

template <class Pane>
class WorkspaceView {
public:
    explicit WorkspaceView(Workspace<Pane> &workspace, ViewConfig config = {})
        : workspace_(workspace)
        , config_(std::move(config))
        , prefix_(config_.prefix) {}

    Workspace<Pane> &workspace() { return workspace_; }
    const ViewConfig &config() const { return config_; }

    // Window size or font changed. Ratios are kept, so splits scale with the window.
    void setWindow(const WindowMetrics &metrics) { (void)metrics; }
    const WindowMetrics &window() const { return metrics_; }

    // The frame to draw now (also applies nothing; call sync() first).
    FramePlan plan() const { return {}; }

    // Resize the views of the active tab's panes to their planned cell size. During a
    // divider drag it is throttled by resizeIntervalMs; the last size always arrives
    // when the drag ends or `force` is set. Returns true if any pane was resized.
    bool sync(std::int64_t nowMs, bool force = false) { (void)nowMs; (void)force; return false; }

    KeyResult key(const KeyChord &chord, std::int64_t nowMs) { (void)chord; (void)nowMs; return {}; }
    // The prefix timed out: returns forwardPrefix if the program still owes a key.
    KeyResult tick(std::int64_t nowMs) { (void)nowMs; return {}; }
    // When tick() next has something to do (prefix deadline, drag throttle), or none.
    std::optional<std::int64_t> nextWakeMs() const { return std::nullopt; }

    MouseResult mouse(const MouseInput &input, std::int64_t nowMs = 0) { (void)input; (void)nowMs; return {}; }
    WheelResult wheel(int x, int y, double amount) { (void)x; (void)y; (void)amount; return {}; }

    // Focus changes since the last call (for focus in/out reports to the programs).
    FocusChange takeFocusChange() { return {}; }

    // True while a divider or scrollbar drag owns the mouse.
    bool dragging() const { return false; }

private:
    Workspace<Pane> &workspace_;
    ViewConfig config_;
    PrefixKeys prefix_;
    WindowMetrics metrics_;
};

} // namespace frontend
