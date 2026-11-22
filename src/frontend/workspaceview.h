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
#include "closeprompt.h"
#include "palette.h"
#include "panetree.h"
#include "prefixkeys.h"
#include "tabpulse.h"
#include "tabstrip.h"
#include "tabtypes.h"
#include "terminalview.h"
#include "ui/layer.h"
#include "ui/shortcuts.h"
#include "workspace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <functional>
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
    bool shareButton = false;    // a Share button in the strip (needs a share probe; panes in the daemon)
    bool paletteButton = false;  // a button at the end of the strip that opens the command palette
    bool windowButtons = false;  // a borderless window: minimise, maximise and close end the strip (section 16)
    bool prefixKeys = true;      // Ctrl+B ... (browser)
    bool desktopShortcuts = true; // Ctrl+Shift+T ... (native)
    PrefixKeys::Config prefix;
    std::int64_t pulseIgnoreAfterInputMs = 5000; // output this soon after input to a pane is its echo, not work
    std::int64_t pulseIgnoreAfterStartMs = 5000; // and this soon after a pane was made
    TabActivity tabActivity = TabActivity::Icon; // what a tab with output nobody asked for shows (--tab-activity)
    std::int64_t whichKeyDelayMs = 500; // after the prefix key, a box lists what can follow (0: never)
    int touchSlop = 16;          // extra pixels around dividers and below the strip for a finger
    // Windows narrower than this many columns show only the focused pane of a
    // tab (0: never); the other panes keep running and come back when the window
    // widens or the focus moves.
    int compactBelowColumns = 0;
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
    bool shared = false; // marked shared: the owner draws a small red mark in its corner
};

// An overlay as it should be drawn: a cell grid at a pixel place (whole cells).
struct LayerPlan {
    PixelRect rect;
    std::shared_ptr<CellBuffer> cells;
    CursorState cursor;
};

struct FramePlan {
    PixelRect stripRect; // empty (h == 0) when the strip is hidden
    TabStrip strip;      // cells of the strip row, columns = stripRect.w / cellWidth
    std::vector<PanePlan> panes;
    std::vector<DividerBox> dividers;
    std::vector<LayerPlan> layers; // bottom first; drawn after the panes
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
    bool touch = false; // a finger: dividers and the strip get a larger hit area
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
    bool refused = false;                // the "+" of the strip could not make a tab
    bool openPalette = false;            // the strip's palette button, or a layer asked for it
    bool copyMode = false;               // a layer chose "Copy / scroll mode": enter it in the focused pane
    std::string action;                  // an application action chosen in a layer
    std::optional<CloseRequest> confirm; // not closed: a program runs there, ask first (answerClose)
};

struct WheelResult {
    bool consumed = false;               // the strip took it (tab switch)
    std::optional<PaneId> pane;          // forward to this pane (under the pointer)
    bool changed = false;
};

struct KeyResult {
    std::vector<CommandCall> commands;   // layout commands that were applied
    std::vector<CommandCall> refused;    // new tab / split that could not be made (limit, too small)
    bool copy = false;                   // Ctrl+Shift+C: copy the focused pane's selection
    bool paste = false;                  // Ctrl+Shift+V
    bool forwardPrefix = false;          // send the prefix key to the focused program first
    bool forwardKey = false;             // then this key
    bool changed = false;                // layout changed: redraw
    bool empty = false;                  // the last tab was closed
    bool openPalette = false;            // create the command palette layer and pushLayer() it
    bool copyMode = false;               // enter copy / scroll mode in the focused pane
    std::string action;                  // an application action chosen in a layer (CommandEntry id)
    std::optional<CloseRequest> confirm; // not closed: a program runs there, ask first (answerClose)
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
    void setWindow(const WindowMetrics &metrics) {
        metrics_ = metrics;
        metrics_.cellWidth = std::max(metrics_.cellWidth, 1);
        metrics_.cellHeight = std::max(metrics_.cellHeight, 1);
        refreshSpec();
    }
    const WindowMetrics &window() const { return metrics_; }
    // The native window is maximised: the middle window button shows "restore" (borderless only).
    void setWindowMaximized(bool maximized) { windowMaximized_ = maximized; }

    // The frame to draw now (applies nothing; call sync() first).
    FramePlan plan(bool withLayers = true) const {
        refreshSpec();
        FramePlan out;
        const int stripH = stripHeight();
        if (stripH > 0) {
            out.stripRect = {0, 0, metrics_.width, stripH};
            out.strip = buildTabStrip(pulsed(workspace_.tabInfos(titleOf)), stripColumns(), config_.palette, stripFirst_,
                                  config_.paletteButton, ShareButton{config_.shareButton && probeShared_, sharedInActiveTab()},
                                  WindowButtons{config_.windowButtons, windowMaximized_});
        }
        const Layout layout = activeLayout();
        out.dividers = layout.dividers;
        out.focus = workspace_.focusedPaneId();
        const int right = metrics_.width;
        for (const auto &box : layout.panes) {
            PanePlan p;
            p.id = box.pane;
            p.rect = box.rect;
            p.columns = box.columns;
            p.rows = box.rows;
            p.focused = box.pane == out.focus;
            p.shared = shared_ && shared_(box.pane);
            if (box.rect.x + box.rect.w >= right) {
                // At the window edge: the strip inside the pane holds the scrollbar.
                p.columns = std::max(1, (box.rect.w - config_.scrollbarStrip) / metrics_.cellWidth);
            }
            else {
                for (const auto &d : layout.dividers) {
                    if (d.direction != SplitDirection::SideBySide || d.rect.x != box.rect.x + box.rect.w) continue;
                    const int top = std::max(d.rect.y, box.rect.y);
                    const int bottom = std::min(d.rect.y + d.rect.h, box.rect.y + box.rect.h);
                    if (bottom <= top) continue;
                    p.scrollbar = {d.rect.x, top, d.rect.w / 2, bottom - top};
                    break;
                }
            }
            p.columns = std::max(p.columns, 1);
            p.rows = std::max(p.rows, 1);
            out.panes.push_back(p);
        }
        if (withLayers) {
            for (const auto &layer : layers_) {
                const Placed placed = place(*layer);
                auto cells = std::make_shared<CellBuffer>(static_cast<std::size_t>(placed.columns),
                                                          static_cast<std::size_t>(placed.rows));
                LayerPlan lp;
                lp.rect = placed.rect;
                lp.cursor.visible = false;
                layer->render(*cells, lp.cursor);
                lp.cells = std::move(cells);
                out.layers.push_back(std::move(lp));
            }
            if (whichKey_ && prefix_.waiting() && layers_.empty()) out.layers.push_back(whichKeyPlan());
        }
        return out;
    }

    // ---- layers (overlays) ----
    // The top layer takes keys, clicks and the wheel before anything else; a click
    // outside it closes it. The owner builds the layer (see ui::CommandPalette).
    void pushLayer(std::unique_ptr<ui::Layer> layer) { layers_.push_back(std::move(layer)); }
    bool hasLayer() const { return !layers_.empty(); }
    void closeLayers() { layers_.clear(); }

    // Resize the views of the active tab's panes to their planned cell size. During a
    // divider drag it is throttled by resizeIntervalMs; the last size always arrives
    // when the drag ends or `force` is set. Returns true if any pane was resized.
    bool sync(std::int64_t nowMs, bool force = false) {
        const FramePlan p = plan(false);
        const bool throttled = !force && !flush_ && drag_ == Drag::Divider &&
                               nowMs - lastSyncMs_ < config_.resizeIntervalMs;
        bool any = false;
        bool pending = false;
        for (const auto &pane : p.panes) {
            Pane *target = workspace_.pane(pane.id);
            if (target == nullptr) continue;
            TerminalView &view = target->view();
            const auto columns = static_cast<std::size_t>(std::min(pane.columns, 65535));
            const auto rows = static_cast<std::size_t>(std::min(pane.rows, 65535));
            if (view.model().columns() == columns && view.model().rows() == rows) continue;
            if (throttled) {
                pending = true;
                continue;
            }
            view.resize(columns, rows);
            any = true;
        }
        pending_ = pending;
        if (!throttled) {
            flush_ = false;
            if (any) lastSyncMs_ = nowMs;
        }
        return any;
    }

    // Keys. Desktop shortcuts first (unless a prefix is pending), then the prefix keys.
    // `repeat` is an auto-repeat of a held key: a held shortcut does nothing more
    // (it must not split twenty times or leak control characters), and other held
    // keys pass through without starting the prefix.
    KeyResult key(const KeyChord &chord, std::int64_t nowMs, bool repeat = false) {
        refreshSpec();
        KeyResult r;
        if (!layers_.empty()) {
            applyOutcome(layers_.back()->key(chord), r);
            return r;
        }
        if (repeat && !prefix_.waiting()) {
            if (!(config_.desktopShortcuts && findDesktopShortcut(chord))) r.forwardKey = true;
            return r;
        }
        if (config_.desktopShortcuts && !prefix_.waiting()) {
            if (const auto s = findDesktopShortcut(chord)) {
                if (s->action == DesktopAction::Copy) {
                    r.copy = true;
                }
                else if (s->action == DesktopAction::Paste) {
                    r.paste = true;
                }
                else {
                    run(s->call, r);
                }
                return r;
            }
        }
        if (!config_.prefixKeys) {
            r.forwardKey = true;
            return r;
        }
        const KeyOutcome o = prefix_.feed(chord, nowMs);
        if (whichKey_ && !prefix_.waiting()) {
            whichKey_ = false;
            r.changed = true;
        }
        r.forwardPrefix = o.forwardPrefix;
        r.forwardKey = o.forwardKey;
        if (o.command) run(*o.command, r);
        return r;
    }
    // The prefix timed out: returns forwardPrefix if the program still owes a key.
    KeyResult tick(std::int64_t nowMs) {
        KeyResult r;
        r.forwardPrefix = prefix_.tick(nowMs).forwardPrefix;
        if (config_.tabActivity != TabActivity::Off && pollOutput(nowMs)) r.changed = true;
        const bool show = prefix_.waiting() && config_.whichKeyDelayMs > 0 && nowMs >= whichKeyAt();
        if (show != whichKey_) {
            whichKey_ = show;
            r.changed = true;
        }
        return r;
    }
    // When tick() or sync() next has something to do (prefix deadline, drag throttle).
    std::optional<std::int64_t> nextWakeMs() const {
        std::optional<std::int64_t> t = prefix_.deadline();
        if (t && !whichKey_ && config_.whichKeyDelayMs > 0) t = std::min(*t, whichKeyAt());
        if (const auto pulse = pulseWakeMs()) t = t ? std::min(*t, *pulse) : *pulse;
        if (pending_ && drag_ == Drag::Divider) {
            const std::int64_t due = lastSyncMs_ + config_.resizeIntervalMs;
            t = t ? std::min(*t, due) : due;
        }
        return t;
    }

    MouseResult mouse(const MouseInput &in, std::int64_t nowMs = 0) {
        (void)nowMs;
        refreshSpec();
        MouseResult r;
        if (!layers_.empty()) return layerMouse(in);
        const FramePlan p = plan(false);

        // An owned drag continues wherever the pointer is.
        if (drag_ == Drag::Divider) {
            r.consumed = true;
            r.cursor = dragAxis_ == SplitDirection::SideBySide ? MouseResult::Cursor::ResizeColumns
                                                               : MouseResult::Cursor::ResizeRows;
            if (in.type != MouseInput::Type::Down) {
                const int pointer = dragAxis_ == SplitDirection::SideBySide ? in.x : in.y;
                r.changed = workspace_.dragDivider(dragSplit_, pointer, in.shift) == Outcome::Done;
            }
            if (in.type == MouseInput::Type::Up) endDrag();
            return r;
        }
        if (drag_ == Drag::Scrollbar) {
            r.consumed = true;
            if (in.type != MouseInput::Type::Down) r.changed = dragScrollbar(in.y);
            if (in.type == MouseInput::Type::Up) endDrag();
            return r;
        }
        if (drag_ == Drag::Selecting) {
            if (const PanePlan *pp = findPane(p, dragPane_)) {
                r.pane = paneMouse(*pp, in, true);
            }
            if (in.type == MouseInput::Type::Up) endDrag();
            return r;
        }

        // The strip.
        const int stripReach = in.touch ? config_.touchSlop : 0;
        if (p.stripRect.h > 0 && in.y >= p.stripRect.y && in.y < p.stripRect.y + p.stripRect.h + stripReach) {
            r.consumed = true;
            if (in.type != MouseInput::Type::Down || (in.button != MouseInput::Button::Left &&
                                                      in.button != MouseInput::Button::Middle)) {
                return r;
            }
            const StripHit hit = p.strip.hit(in.x / metrics_.cellWidth);
            const std::size_t tab = hit.tab < 0 ? 0 : static_cast<std::size_t>(hit.tab);
            if (in.button == MouseInput::Button::Middle) {
                if (hit.kind == StripHit::Kind::Tab || hit.kind == StripHit::Kind::Close) {
                    closeTabGuarded(tab, r);
                }
                return r;
            }
            switch (hit.kind) {
            case StripHit::Kind::Tab:
                finish(workspace_.select(tab), r);
                break;
            case StripHit::Kind::Close:
                closeTabGuarded(tab, r);
                break;
            case StripHit::Kind::NewTab:
            {
                const Outcome made = workspace_.apply(Command::NewTab);
                r.refused = made == Outcome::Ignored;
                finish(made, r);
            }
                break;
            case StripHit::Kind::Palette:
                r.openPalette = true;
                r.changed = true;
                break;
            case StripHit::Kind::Share:
                r.action = sharedInActiveTab() > 0 ? "share.tab.stop" : "share.tab";
                r.changed = true;
                break;
            case StripHit::Kind::ScrollLeft:
                stripFirst_ = std::max(0, std::max(stripFirst_, p.strip.firstVisible) - 1);
                r.changed = true;
                break;
            case StripHit::Kind::ScrollRight:
                stripFirst_ = std::max(stripFirst_, p.strip.firstVisible) + 1;
                r.changed = true;
                break;
            case StripHit::Kind::Minimize:
                r.action = "window.minimize";
                break;
            case StripHit::Kind::Maximize:
                r.action = "window.maximize";
                break;
            case StripHit::Kind::CloseWindow:
                r.action = "window.close";
                break;
            case StripHit::Kind::None:
                // A double click on the handle of a borderless window toggles maximise.
                if (config_.windowButtons && in.clicks == 2 && in.button == MouseInput::Button::Left) {
                    r.action = "window.maximize";
                }
                break;
            }
            return r;
        }

        // Dividers.
        const Layout layout = activeLayout();
        const int d = in.touch ? dividerNear(layout, in.x, in.y, config_.touchSlop)
                               : layout.dividerAt(in.x, in.y);
        if (d >= 0 && (in.touch || !overScrollbar(p, in.x, in.y))) {
            const auto idx = static_cast<std::size_t>(d);
            r.consumed = true;
            r.cursor = layout.dividers[idx].direction == SplitDirection::SideBySide
                           ? MouseResult::Cursor::ResizeColumns
                           : MouseResult::Cursor::ResizeRows;
            if (in.type == MouseInput::Type::Down && in.button == MouseInput::Button::Left) {
                drag_ = Drag::Divider;
                dragSplit_ = layout.dividerSplits[idx];
                dragAxis_ = layout.dividers[idx].direction;
                const int pointer = dragAxis_ == SplitDirection::SideBySide ? in.x : in.y;
                r.changed = workspace_.dragDivider(dragSplit_, pointer, in.shift) == Outcome::Done;
            }
            return r;
        }

        // Scrollbars.
        if (const PanePlan *pp = scrollbarAt(p, in.x, in.y)) {
            r.consumed = true;
            if (in.type == MouseInput::Type::Down && in.button == MouseInput::Button::Left) {
                drag_ = Drag::Scrollbar;
                dragPane_ = pp->id;
                dragTrack_ = scrollTrack(*pp);
                r.changed = dragScrollbar(in.y);
            }
            return r;
        }

        // A pane.
        const PaneId id = layout.paneAt(in.x, in.y);
        const PanePlan *pp = findPane(p, id);
        if (pp == nullptr) return r;
        bool focusedNow = false;
        if (in.type == MouseInput::Type::Down && pp->id != workspace_.focusedPaneId()) {
            focusedNow = workspace_.focusPane(pp->id) == Outcome::Done;
            r.changed = focusedNow;
        }
        r.pane = paneMouse(*pp, in, false);
        r.pane->focusedByThis = focusedNow;
        if (in.type == MouseInput::Type::Down) {
            drag_ = Drag::Selecting;
            dragPane_ = pp->id;
        }
        return r;
    }

    // The wheel goes to the pane under the pointer; over the strip it switches tabs.
    WheelResult wheel(int x, int y, double amount) {
        refreshSpec();
        WheelResult r;
        if (!layers_.empty()) {
            place(*layers_.back()); // Sized before use.
            layers_.back()->wheel(amount);
            r.consumed = true;
            r.changed = true;
            return r;
        }
        const int stripH = stripHeight();
        if (stripH > 0 && y >= 0 && y < stripH) {
            r.consumed = true;
            if (amount != 0) {
                r.changed = workspace_.apply(amount > 0 ? Command::PreviousTab : Command::NextTab) ==
                            Outcome::Done;
            }
            return r;
        }
        const PaneId id = activeLayout().paneAt(x, y);
        if (id != 0) r.pane = id;
        return r;
    }

    // Focus changes since the last call (for focus in/out reports to the programs).
    FocusChange takeFocusChange() {
        const PaneId now = workspace_.focusedPaneId();
        FocusChange c;
        if (now != lastFocus_) {
            c.lost = lastFocus_;
            c.gained = now;
            lastFocus_ = now;
        }
        return c;
    }

    // A key press is pending after the prefix key (the next printable key is a command).
    bool prefixWaiting() const { return prefix_.waiting(); }
    void setTabActivity(TabActivity mode) {
        config_.tabActivity = mode;
        lastPulse_ = iconBits_ = 0;
        iconSecond_ = -1;
    }
    // Tab `index` had output that counts lately (as of the last tick).
    bool working(std::size_t index) const {
        if (config_.tabActivity == TabActivity::Off) return false;
        if (config_.tabActivity == TabActivity::Icon) return index < 64 && ((iconBits_ >> index) & 1) != 0;
        return tabWorking(index, nowMs_);
    }
    bool whichKeyShown() const { return whichKey_ && prefix_.waiting(); }

    // The pane under a point and where in it, without changing anything (hover, wheel).
    std::optional<PaneMouse> pointerPane(int x, int y) const {
        refreshSpec();
        const FramePlan p = plan();
        const PanePlan *pp = findPane(p, activeLayout().paneAt(x, y));
        if (pp == nullptr) return std::nullopt;
        MouseInput in;
        in.x = x;
        in.y = y;
        return paneMouse(*pp, in, false);
    }

    // Whether a finger at this point would grab a divider (so the owner can
    // route that touch to mouse() instead of a scroll gesture).
    bool nearDivider(int x, int y) const {
        refreshSpec();
        return dividerNear(activeLayout(), x, y, config_.touchSlop) >= 0;
    }

    // True while a divider or scrollbar drag owns the mouse.
    bool dragging() const { return drag_ == Drag::Divider || drag_ == Drag::Scrollbar; }

    // Closing with a program running (docs/TABS_PLAN.md section 9). The probe says what a pane
    // runs when its foreground process is not a shell (nullopt: nothing to ask about). Without
    // a probe nothing is asked, as before. Closing a pane or tab (keys, the strip's x and
    // middle click) that holds such a pane does not close: the result carries `confirm` and
    // the same request is pending until answerClose(). A pane whose process has ended is
    // never asked about (it is closed through Workspace::closePaneAnywhere).
    using BusyProbe = std::function<std::optional<std::string>(PaneId)>;
    void setBusyProbe(BusyProbe probe) { busy_ = std::move(probe); }
    // Which panes are shared (the daemon's mark). Without a probe nothing is shown shared and the
    // Share button is not offered. Clicking the button returns the action "share.tab" (nothing in
    // the active tab is shared: share it) or "share.tab.stop" (stop sharing it).
    using ShareProbe = std::function<bool(PaneId)>;
    void setShareProbe(ShareProbe probe) {
        shared_ = std::move(probe);
        probeShared_ = bool(shared_);
    }
    // Shared panes in the active tab.
    int sharedInActiveTab() const {
        int n = 0;
        if (shared_) {
            workspace_.forEachPane([&](PaneId id, const Pane &, bool visible) {
                if (visible && shared_(id)) ++n;
            });
        }
        return n;
    }
    const std::optional<CloseRequest> &pendingClose() const { return pendingClose_; }
    // Closing the whole window: nullopt when it is fine to quit now; otherwise the request
    // (also pending, answer it with answerClose; Close then means "quit", nothing is changed here).
    std::optional<CloseRequest> windowClose() {
        pendingClose_.reset();
        std::vector<RunningProgram> running;
        for (std::size_t t = 0; t < workspace_.tabCount(); ++t) collectBusy(*workspace_.tab(t), running);
        if (running.empty()) return std::nullopt;
        CloseRequest req;
        req.scope = CloseScope::Window;
        req.running = std::move(running);
        pendingClose_ = req;
        return req;
    }
    // The answer to the pending request. Cancel (or no request) changes nothing. Close performs
    // the close of that tab or pane if it still exists and returns what changed.
    KeyResult answerClose(CloseAnswer answer) {
        KeyResult r;
        const auto req = pendingClose_;
        pendingClose_.reset();
        if (!req || answer != CloseAnswer::Close || req->scope == CloseScope::Window) return r;
        Outcome o = Outcome::Ignored;
        if (req->scope == CloseScope::Tab) {
            const int i = workspace_.indexOfTab(req->tab);
            if (i >= 0) o = workspace_.closeTabAt(static_cast<std::size_t>(i));
        }
        else {
            o = workspace_.closePaneAnywhere(req->pane);
        }
        if (o == Outcome::Ignored) return r;
        r.changed = true;
        r.empty = o == Outcome::Empty;
        refreshSpec();
        return r;
    }

private:
    enum class Drag { None, Divider, Scrollbar, Selecting };

    // Looks at every pane's output counter; true when the pulse of the strip changed since the last
    // look (a tab began or stopped working, or the phase turned while one works).
    bool pollOutput(std::int64_t nowMs) {
        nowMs_ = nowMs;
        workspace_.forEachPane([&](PaneId id, Pane &pane, bool) {
            output_.update(id, pane.view().outputBytes(), pane.view().inputBytes(), nowMs,
                           {config_.pulseIgnoreAfterInputMs, config_.pulseIgnoreAfterStartMs});
        });
        if (config_.tabActivity == TabActivity::Icon) {
            // The icon is looked at once a second: it shows or hides at most that often.
            const std::int64_t second = nowMs / TabPulse::kPeriodMs;
            if (second == iconSecond_) return false;
            iconSecond_ = second;
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < workspace_.tabCount() && i < 64; ++i) {
                if (tabWorking(i, nowMs)) bits |= std::uint64_t(1) << i;
            }
            const bool changed = bits != iconBits_;
            iconBits_ = bits;
            return changed;
        }
        const std::uint64_t now = pulseState(nowMs);
        const bool changed = now != lastPulse_;
        lastPulse_ = now;
        return changed;
    }
    // Bit i: tab i works and is in the dark half. Zero when nothing works.
    std::uint64_t pulseState(std::int64_t nowMs) const {
        std::uint64_t bits = 0;
        if (!TabPulse::dark(nowMs)) return bits;
        for (std::size_t i = 0; i < workspace_.tabCount() && i < 64; ++i) {
            if (tabWorking(i, nowMs)) bits |= std::uint64_t(1) << i;
        }
        return bits;
    }
    bool tabWorking(std::size_t index, std::int64_t nowMs) const {
        const auto *tab = workspace_.tab(index);
        if (!tab) return false;
        for (const PaneId id : tab->tree.ids()) {
            if (output_.recent(id, nowMs)) return true;
        }
        return false;
    }
    // The strip's tabs with the pulse applied for the time of the last tick.
    std::vector<TabInfo> pulsed(std::vector<TabInfo> infos) const {
        if (config_.tabActivity == TabActivity::Off) return infos;
        for (std::size_t i = 0; i < infos.size(); ++i) {
            if (config_.tabActivity == TabActivity::Icon) {
                infos[i].working = i < 64 && ((iconBits_ >> i) & 1) != 0;
            }
            else {
                infos[i].pulseDark = TabPulse::dark(nowMs_) && tabWorking(i, nowMs_);
            }
        }
        return infos;
    }
    // When the pulse next needs a look: a working tab flips at the next period boundary.
    std::optional<std::int64_t> pulseWakeMs() const {
        if (config_.tabActivity == TabActivity::Off) return std::nullopt;
        if (config_.tabActivity == TabActivity::Icon && iconBits_ != 0) return TabPulse::nextChange(nowMs_);
        for (std::size_t i = 0; i < workspace_.tabCount(); ++i) {
            if (tabWorking(i, nowMs_)) return TabPulse::nextChange(nowMs_);
        }
        return std::nullopt;
    }

    // When the which-key box appears for the prefix now waiting.
    std::int64_t whichKeyAt() const {
        return prefix_.deadline().value_or(0) - config_.prefix.timeoutMs + config_.whichKeyDelayMs;
    }
    void showShortcuts() {
        // Desktop keys only where they work; the prefix ones likewise.
        layers_.push_back(std::make_unique<ui::ShortcutsView>(buildCommandRegistry(config_.prefix),
                                                              config_.desktopShortcuts, config_.prefixKeys));
    }
    // The which-key box: bottom right, above the last row.
    LayerPlan whichKeyPlan() const {
        ui::WhichKey box(config_.prefix);
        const int windowColumns = std::max(metrics_.width / metrics_.cellWidth, 1);
        const int windowRows = std::max(metrics_.height / metrics_.cellHeight, 1);
        const ui::CellSize size = box.wanted(windowColumns, windowRows);
        const int columns = std::clamp(size.columns, 1, windowColumns);
        const int rows = std::clamp(size.rows, 1, windowRows);
        box.resize(columns, rows);
        auto cells = std::make_shared<CellBuffer>(std::size_t(columns), std::size_t(rows));
        LayerPlan lp;
        lp.cursor.visible = false;
        box.render(*cells, lp.cursor);
        lp.cells = std::move(cells);
        lp.rect = {(windowColumns - columns) * metrics_.cellWidth,
                   std::max(windowRows - rows - 1, 0) * metrics_.cellHeight, columns * metrics_.cellWidth,
                   rows * metrics_.cellHeight};
        return lp;
    }

    struct Placed {
        PixelRect rect;
        int columns = 0;
        int rows = 0;
    };
    // Sizes a layer for the window and says where it goes: centred, just below the
    // strip row.
    Placed place(ui::Layer &layer) const {
        const int windowColumns = std::max(metrics_.width / metrics_.cellWidth, 1);
        const int windowRows = std::max(metrics_.height / metrics_.cellHeight, 1);
        const ui::CellSize size = layer.wanted(windowColumns, windowRows);
        Placed p;
        p.columns = std::clamp(size.columns, 1, windowColumns);
        p.rows = std::clamp(size.rows, 1, windowRows);
        layer.resize(p.columns, p.rows);
        const int column = (windowColumns - p.columns) / 2;
        const int row = std::min(1, windowRows - p.rows);
        p.rect = {column * metrics_.cellWidth, row * metrics_.cellHeight, p.columns * metrics_.cellWidth,
                  p.rows * metrics_.cellHeight};
        return p;
    }

    // What a layer answered: closing it, a layout command, an application action.
    template <class Result>
    void applyOutcome(const ui::LayerOutcome &o, Result &r) {
        r.changed = true;
        if (o.close && !layers_.empty()) layers_.pop_back();
        if (!o.action.empty()) r.action = o.action;
        if (!o.call) return;
        if (o.call->command == Command::OpenPalette) {
            r.openPalette = true;
            return;
        }
        if (o.call->command == Command::CopyMode) {
            r.copyMode = true;
            return;
        }
        if (o.call->command == Command::ShowShortcuts) {
            showShortcuts();
            return;
        }
        applyCall(*o.call, r);
    }
    void applyCall(const CommandCall &call, KeyResult &r) { run(call, r); }
    void applyCall(const CommandCall &call, MouseResult &r) {
        finish(workspace_.apply(call.command, call.argument), r);
    }

    MouseResult layerMouse(const MouseInput &in) {
        MouseResult r;
        r.consumed = true;
        const Placed placed = place(*layers_.back());
        if (in.type != MouseInput::Type::Down || in.button != MouseInput::Button::Left) return r;
        if (Layout::contains(placed.rect, in.x, in.y)) {
            const int column = (in.x - placed.rect.x) / metrics_.cellWidth;
            const int row = (in.y - placed.rect.y) / metrics_.cellHeight;
            applyOutcome(layers_.back()->click(column, row), r);
        }
        else {
            layers_.pop_back(); // A click outside dismisses the overlay.
            r.changed = true;
        }
        return r;
    }

    // The layout that is shown: the active tab's, or only its focused pane in a
    // window narrower than compactBelowColumns.
    Layout activeLayout() const {
        Layout full = workspace_.layoutActive();
        if (config_.compactBelowColumns <= 0 || full.panes.size() < 2 ||
            metrics_.width / metrics_.cellWidth >= config_.compactBelowColumns) {
            return full;
        }
        const PaneId focus = workspace_.focusedPaneId();
        const PaneBox *focused = full.box(focus);
        if (focused == nullptr) return full;
        Layout one;
        PixelRect area{0, 0, 0, 0};
        const LayoutSpec &spec = workspace_.layoutSpec();
        area = spec.area;
        PaneBox b;
        b.pane = focus;
        b.rect = area;
        b.columns = area.w / metrics_.cellWidth;
        b.rows = area.h / metrics_.cellHeight;
        const int usedW = b.columns * metrics_.cellWidth;
        const int usedH = b.rows * metrics_.cellHeight;
        b.leftover[0] = {area.x + usedW, area.y, area.w - usedW, area.h};
        b.leftover[1] = {area.x, area.y + usedH, usedW, area.h - usedH};
        one.panes.push_back(b);
        return one;
    }

    // The divider whose gutter, grown by `slop` pixels on every side, holds the
    // point (the nearest if several), or -1.
    static int dividerNear(const Layout &layout, int x, int y, int slop) {
        int best = -1;
        long long bestDistance = 0;
        for (std::size_t i = 0; i < layout.dividers.size(); ++i) {
            const PixelRect &g = layout.dividers[i].rect;
            const PixelRect grown{g.x - slop, g.y - slop, g.w + 2 * slop, g.h + 2 * slop};
            if (!Layout::contains(grown, x, y)) continue;
            const long long dx = x - (g.x + g.w / 2);
            const long long dy = y - (g.y + g.h / 2);
            const long long distance = dx * dx + dy * dy;
            if (best < 0 || distance < bestDistance) {
                best = static_cast<int>(i);
                bestDistance = distance;
            }
        }
        return best;
    }

    static std::string titleOf(const Pane &pane) { return pane.view().title(); }

    int stripHeight() const {
        const bool show = config_.showStripWithOneTab || workspace_.tabCount() > 1;
        return show ? metrics_.cellHeight : 0;
    }
    int stripColumns() const { return metrics_.width / metrics_.cellWidth; }

    void refreshSpec() const {
        LayoutSpec s;
        const int stripH = stripHeight();
        s.area = {0, stripH, std::max(metrics_.width, 0), std::max(metrics_.height - stripH, 0)};
        s.cellWidth = metrics_.cellWidth;
        s.cellHeight = metrics_.cellHeight;
        s.dividerThickness = config_.dividerThickness;
        // A pane at the window edge loses the scrollbar strip: it must still keep the minimum.
        s.minColumns = kMinPaneColumns + (config_.scrollbarStrip + metrics_.cellWidth - 1) / metrics_.cellWidth;
        workspace_.setLayoutSpec(s);
    }

    void collectBusy(const typename Workspace<Pane>::Tab &tab, std::vector<RunningProgram> &out) const {
        if (!busy_) return;
        for (const PaneId id : tab.tree.ids()) {
            if (auto text = busy_(id)) out.push_back({id, std::move(*text)});
        }
    }
    // The request for closing a tab (all its panes) or one pane, nullopt when nothing runs there.
    std::optional<CloseRequest> guardTab(std::size_t index) {
        const auto *tab = workspace_.tab(index);
        if (!tab || !busy_) return std::nullopt;
        CloseRequest req;
        req.scope = CloseScope::Tab;
        req.tab = tab->id;
        collectBusy(*tab, req.running);
        return req.running.empty() ? std::nullopt : std::optional<CloseRequest>(req);
    }
    std::optional<CloseRequest> guardPane(PaneId id) {
        if (!busy_ || !workspace_.activeTab()) return std::nullopt;
        auto text = busy_(id);
        if (!text) return std::nullopt;
        CloseRequest req;
        req.scope = CloseScope::Pane;
        req.tab = workspace_.activeTab()->id;
        req.pane = id;
        req.running.push_back({id, std::move(*text)});
        return req;
    }

    void run(const CommandCall &call, KeyResult &r) {
        if (call.command == Command::OpenPalette) {
            r.openPalette = true;
            r.changed = true;
            r.commands.push_back(call);
            return;
        }
        if (call.command == Command::CopyMode) {
            r.copyMode = true;
            r.changed = true;
            r.commands.push_back(call);
            return;
        }
        if (call.command == Command::ShowShortcuts) {
            showShortcuts();
            r.changed = true;
            r.commands.push_back(call);
            return;
        }
        if (call.command == Command::CloseTab || call.command == Command::ClosePane) {
            auto req = call.command == Command::CloseTab ? guardTab(workspace_.activeIndex())
                                                         : guardPane(workspace_.focusedPaneId());
            if (req) {
                pendingClose_ = req;
                r.confirm = std::move(req);
                return;
            }
        }
        const Outcome o = workspace_.apply(call.command, call.argument);
        if (o == Outcome::Ignored) {
            if (call.command == Command::NewTab || call.command == Command::SplitSideBySide ||
                call.command == Command::SplitStacked) {
                r.refused.push_back(call);
            }
            return;
        }
        r.commands.push_back(call);
        r.changed = true;
        if (o == Outcome::Empty) r.empty = true;
        refreshSpec(); // the strip may appear or disappear
    }
    void closeTabGuarded(std::size_t tab, MouseResult &r) {
        if (auto req = guardTab(tab)) {
            pendingClose_ = req;
            r.confirm = std::move(req);
            r.consumed = true;
            return;
        }
        finish(workspace_.closeTabAt(tab), r);
    }
    void finish(Outcome o, MouseResult &r) {
        if (o == Outcome::Ignored) return;
        r.changed = true;
        if (o == Outcome::Empty) r.empty = true;
        refreshSpec();
    }
    void endDrag() {
        if (drag_ == Drag::Divider) flush_ = true;
        drag_ = Drag::None;
        dragPane_ = 0;
        dragSplit_ = 0;
    }

    static const PanePlan *findPane(const FramePlan &p, PaneId id) {
        for (const auto &pane : p.panes) {
            if (pane.id == id) return &pane;
        }
        return nullptr;
    }

    static bool inside(const PixelRect &r, int x, int y) { return Layout::contains(r, x, y); }

    // The scrollbar track of a pane: the gutter half, or the strip inside its right edge.
    PixelRect scrollTrack(const PanePlan &pp) const {
        if (pp.scrollbar.w > 0) return pp.scrollbar;
        const int w = std::min(config_.scrollbarStrip, pp.rect.w);
        return {pp.rect.x + pp.rect.w - w, pp.rect.y, w, pp.rect.h};
    }
    bool scrollbarVisible(const PanePlan &pp) const {
        Pane *pane = workspace_.pane(pp.id);
        if (pane == nullptr) return false;
        const TerminalModel &model = pane->view().model();
        return !model.alternateScreen() && model.frame(metrics_.cellHeight).scrollbar.visible;
    }
    const PanePlan *scrollbarAt(const FramePlan &p, int x, int y) const {
        for (const auto &pp : p.panes) {
            if (inside(scrollTrack(pp), x, y) && scrollbarVisible(pp)) return &pp;
        }
        return nullptr;
    }
    bool overScrollbar(const FramePlan &p, int x, int y) const { return scrollbarAt(p, x, y) != nullptr; }

    // Move the thumb's centre to pointer y. False if nothing moved.
    bool dragScrollbar(int y) {
        Pane *pane = workspace_.pane(dragPane_);
        if (pane == nullptr || dragTrack_.h <= 0) return false;
        TerminalModel &model = pane->view().model();
        const auto sb = model.frame(metrics_.cellHeight).scrollbar;
        if (!sb.visible || !(sb.size > 0.0f) || sb.size >= 1.0f) return false;
        const double size = static_cast<double>(sb.size);
        const double position = static_cast<double>(sb.position);
        const double track = dragTrack_.h;
        const double thumb = std::clamp(track * size, std::min(16.0, track), track);
        const double travel = track - thumb;
        if (travel <= 0) return false;
        const double wanted = std::clamp((y - dragTrack_.y - thumb / 2) / travel, 0.0, 1.0);
        const double total = static_cast<double>(model.rows()) / size;
        const double rows = (wanted - position) * (total - static_cast<double>(model.rows()));
        if (std::abs(rows) < 0.01) return false;
        model.scrollBy(-rows); // positive scrolls towards older output
        return true;
    }

    PaneMouse paneMouse(const PanePlan &pp, const MouseInput &in, bool captured) const {
        PaneMouse m;
        m.pane = pp.id;
        m.x = in.x - pp.rect.x;
        m.y = in.y - pp.rect.y;
        if (Pane *pane = workspace_.pane(pp.id)) {
            const TerminalModel &model = pane->view().model();
            const double w = static_cast<double>(pp.columns) * metrics_.cellWidth;
            const double h = static_cast<double>(pp.rows) * metrics_.cellHeight;
            m.cell = cellAt(model, m.x, m.y, metrics_.cellWidth, metrics_.cellHeight, w, h, false);
            const auto c = cellAt(model, m.x, m.y, metrics_.cellWidth, metrics_.cellHeight, w, h, true);
            if (c) m.clamped = *c;
        }
        (void)captured;
        return m;
    }

    Workspace<Pane> &workspace_;
    BusyProbe busy_;
    ShareProbe shared_;
    bool probeShared_ = false;
    std::optional<CloseRequest> pendingClose_;
    ViewConfig config_;
    bool whichKey_ = false; // the which-key box is showing
    OutputWatch output_;
    std::int64_t nowMs_ = 0;
    std::uint64_t lastPulse_ = 0;
    std::uint64_t iconBits_ = 0;      // tab i shows the work icon (tabActivity = icon)
    std::int64_t iconSecond_ = -1;    // the second the icon was last looked at
    mutable std::vector<std::unique_ptr<ui::Layer>> layers_; // bottom first; resized by place()
    PrefixKeys prefix_;
    WindowMetrics metrics_;
    Drag drag_ = Drag::None;
    SplitId dragSplit_ = 0;
    SplitDirection dragAxis_ = SplitDirection::SideBySide;
    PaneId dragPane_ = 0;
    PixelRect dragTrack_;
    int stripFirst_ = 0;
    bool windowMaximized_ = false;
    std::int64_t lastSyncMs_ = -1000000;
    bool pending_ = false;
    bool flush_ = false;
    PaneId lastFocus_ = 0;
};

} // namespace frontend
