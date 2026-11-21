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

#include <algorithm>
#include <cmath>
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
    void setWindow(const WindowMetrics &metrics) {
        metrics_ = metrics;
        metrics_.cellWidth = std::max(metrics_.cellWidth, 1);
        metrics_.cellHeight = std::max(metrics_.cellHeight, 1);
        refreshSpec();
    }
    const WindowMetrics &window() const { return metrics_; }

    // The frame to draw now (applies nothing; call sync() first).
    FramePlan plan() const {
        refreshSpec();
        FramePlan out;
        const int stripH = stripHeight();
        if (stripH > 0) {
            out.stripRect = {0, 0, metrics_.width, stripH};
            out.strip = buildTabStrip(workspace_.tabInfos(titleOf), stripColumns(), config_.palette, stripFirst_);
        }
        const Layout layout = workspace_.layoutActive();
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
        return out;
    }

    // Resize the views of the active tab's panes to their planned cell size. During a
    // divider drag it is throttled by resizeIntervalMs; the last size always arrives
    // when the drag ends or `force` is set. Returns true if any pane was resized.
    bool sync(std::int64_t nowMs, bool force = false) {
        const FramePlan p = plan();
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
    KeyResult key(const KeyChord &chord, std::int64_t nowMs) {
        refreshSpec();
        KeyResult r;
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
        r.forwardPrefix = o.forwardPrefix;
        r.forwardKey = o.forwardKey;
        if (o.command) run(*o.command, r);
        return r;
    }
    // The prefix timed out: returns forwardPrefix if the program still owes a key.
    KeyResult tick(std::int64_t nowMs) {
        KeyResult r;
        r.forwardPrefix = prefix_.tick(nowMs).forwardPrefix;
        return r;
    }
    // When tick() or sync() next has something to do (prefix deadline, drag throttle).
    std::optional<std::int64_t> nextWakeMs() const {
        std::optional<std::int64_t> t = prefix_.deadline();
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
        const FramePlan p = plan();

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
        if (p.stripRect.h > 0 && in.y >= p.stripRect.y && in.y < p.stripRect.y + p.stripRect.h) {
            r.consumed = true;
            if (in.type != MouseInput::Type::Down || (in.button != MouseInput::Button::Left &&
                                                      in.button != MouseInput::Button::Middle)) {
                return r;
            }
            const StripHit hit = p.strip.hit(in.x / metrics_.cellWidth);
            const std::size_t tab = hit.tab < 0 ? 0 : static_cast<std::size_t>(hit.tab);
            if (in.button == MouseInput::Button::Middle) {
                if (hit.kind == StripHit::Kind::Tab || hit.kind == StripHit::Kind::Close) {
                    finish(workspace_.closeTabAt(tab), r);
                }
                return r;
            }
            switch (hit.kind) {
            case StripHit::Kind::Tab:
                finish(workspace_.select(tab), r);
                break;
            case StripHit::Kind::Close:
                finish(workspace_.closeTabAt(tab), r);
                break;
            case StripHit::Kind::NewTab:
                finish(workspace_.apply(Command::NewTab), r);
                break;
            case StripHit::Kind::ScrollLeft:
                stripFirst_ = std::max(0, std::max(stripFirst_, p.strip.firstVisible) - 1);
                r.changed = true;
                break;
            case StripHit::Kind::ScrollRight:
                stripFirst_ = std::max(stripFirst_, p.strip.firstVisible) + 1;
                r.changed = true;
                break;
            case StripHit::Kind::None:
                break;
            }
            return r;
        }

        // Dividers.
        const Layout layout = workspace_.layoutActive();
        const int d = layout.dividerAt(in.x, in.y);
        if (d >= 0 && !overScrollbar(p, in.x, in.y)) {
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
        const int stripH = stripHeight();
        if (stripH > 0 && y >= 0 && y < stripH) {
            r.consumed = true;
            if (amount != 0) {
                r.changed = workspace_.apply(amount > 0 ? Command::PreviousTab : Command::NextTab) ==
                            Outcome::Done;
            }
            return r;
        }
        const PaneId id = workspace_.layoutActive().paneAt(x, y);
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

    // True while a divider or scrollbar drag owns the mouse.
    bool dragging() const { return drag_ == Drag::Divider || drag_ == Drag::Scrollbar; }

private:
    enum class Drag { None, Divider, Scrollbar, Selecting };

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

    void run(const CommandCall &call, KeyResult &r) {
        const Outcome o = workspace_.apply(call.command, call.argument);
        if (o == Outcome::Ignored) return;
        r.commands.push_back(call);
        r.changed = true;
        if (o == Outcome::Empty) r.empty = true;
        refreshSpec(); // the strip may appear or disappear
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
    ViewConfig config_;
    PrefixKeys prefix_;
    WindowMetrics metrics_;
    Drag drag_ = Drag::None;
    SplitId dragSplit_ = 0;
    SplitDirection dragAxis_ = SplitDirection::SideBySide;
    PaneId dragPane_ = 0;
    PixelRect dragTrack_;
    int stripFirst_ = 0;
    std::int64_t lastSyncMs_ = -1000000;
    bool pending_ = false;
    bool flush_ = false;
    PaneId lastFocus_ = 0;
};

} // namespace frontend
