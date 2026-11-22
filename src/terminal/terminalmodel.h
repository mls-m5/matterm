#pragma once

#include "renderframe.h"
#include "scrollviewport.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

// CPU-only terminal behavior, available to library clients without a PTY or VT
// parser. One code point per cell for now; no wide characters or text clusters.
class TerminalModel {
public:
    enum class EraseMode { ToEnd, ToStart, All };

    TerminalModel(std::size_t columns,
                  std::size_t rows,
                  std::size_t scrollbackRows = 1000);
    ~TerminalModel();
    TerminalModel(const TerminalModel &) = delete;
    TerminalModel &operator=(const TerminalModel &) = delete;
    TerminalModel(TerminalModel &&) = delete;
    TerminalModel &operator=(TerminalModel &&) = delete;

    std::size_t columns() const noexcept;
    std::size_t rows() const noexcept;
    std::size_t historySize() const noexcept;
    bool alternateScreen() const noexcept;
    const FChar &cell(std::size_t column, std::size_t row) const;
    CursorState cursor() const noexcept;
    void setCursor(CursorState cursor);
    void moveCursor(int column, int row);
    void setColors(Rgb8 foreground, Rgb8 background);
    void setColorReferences(ColorReference foreground,
                            ColorReference background);
    void setAttributes(CellAttribute attributes);
    void setCursorVisible(bool visible);
    void setCursorAppearance(CursorState::Shape shape, bool blink);
    // Per-screen save slot: cursor, colors, and pending wrap. Restore clamps
    // coordinates after resize. Calling restore before save is a no-op.
    void saveCursor();
    void restoreCursor();

    // UTF-32, not UTF-8/ANSI. Handles CR, LF, BS and horizontal tab (8-cell
    // stops). LF preserves the column; use CR LF for a new line at column zero.
    // Wrap is deferred until another printable character follows the last
    // column. Unsupported controls are ignored; invalid scalars become U+FFFD.
    void write(std::u32string_view text);
    void carriageReturn();
    void lineFeed();
    void eraseLine(EraseMode mode = EraseMode::ToEnd);
    void eraseDisplay(EraseMode mode = EraseMode::ToEnd);
    // ED3: discard primary saved lines, keeping the live screen, rendition,
    // modes and cursor. A no-op on the history-free alternate screen.
    void eraseScrollback();
    void eraseCharacters(int count = 1);
    void insertCharacters(int count = 1);
    void deleteCharacters(int count = 1);
    void insertLines(int count = 1);
    void deleteLines(int count = 1);
    void scrollUp(int count = 1);
    void scrollDown(int count = 1);
    void reverseIndex();

    // Inclusive zero-based margins; invalid regions leave state unchanged.
    bool setScrollRegion(int top, int bottom);
    int scrollTop() const noexcept;
    int scrollBottom() const noexcept;
    void setOriginMode(bool enabled);
    bool originMode() const noexcept;
    // VT addressing is relative to margins only when origin mode is enabled.
    void addressCursor(int column, int row);
    void setAutoWrap(bool enabled);
    void setInsertMode(bool enabled);
    void tabForward(int count = 1);
    void tabBackward(int count = 1);
    void setTabStop();
    void clearTabStops(bool all = false);
    void reset();

    // Entry clears/homes the alternate buffer. Repeated entry/exit is a no-op.
    // Primary cells, history, cursor, colors, and viewport survive switching.
    // This is a direct API policy, not an implementation of all DEC mode
    // variants.
    void setAlternateScreen(bool enabled);

    // Positive dimensions only. On a width change the primary screen's saved rows are laid out
    // again (soft-wrapped lines are joined and wrapped at the new width, see
    // docs/SCROLLING_AND_REFLOW.md); the live screen rows and the alternate screen clip/pad, programs
    // redraw them. The newest saved rows are laid out at once, older ones in slices (reflowPending,
    // advanceViewport) so a drag stays cheap. Height shrink
    // first drops blank rows below the cursor, then keeps the bottom screen
    // rows (primary top rows become history). Growth reveals primary saved
    // rows first, then adds blanks; alternate growth only adds blanks.
    // Preserves both buffers; resets margins/origin to full screen. Allocation
    // failure leaves both unchanged.
    void resize(std::size_t columns, std::size_t rows);
    // Older saved rows still wait to be laid out after a width change; advanceViewport() works through
    // them once the size has been quiet for a moment. finishReflow() does all of it now.
    bool reflowPending() const noexcept;
    void finishReflow();

    struct TextCell {
        std::size_t row, column;
    };
    // Row IDs survive full-screen scrolling until evicted. Resize, reset,
    // screen switches and regional row movement invalidate the selection epoch.
    std::size_t firstRetainedRow() const noexcept;
    std::size_t selectionEpoch() const noexcept;
    // Read retained text without changing the viewport or borrowing a frame.
    // Invalid row/cell IDs return nullopt/false. A wrapped row joins the next.
    std::optional<char32_t> textCodePoint(TextCell cell) const noexcept;
    bool textRowWrapped(std::size_t row) const noexcept;
    // For each row of the current frame (frame(cellHeight).cells): does it continue on the next
    // row? Fill RenderFrame::wrapMarks with it when a front end wants the wrapped-line marker.
    std::vector<bool> viewWrapMarks() const;
    // Inclusive endpoints, either order. UTF-8, no newline across soft wraps.
    // Invalid or over-limit ranges return nullopt.
    std::optional<std::string> copyText(TextCell first,
                                        TextCell last,
                                        std::size_t byteLimit) const;

    // Scrolling stops on whole rows (default: pixel scrolling, it may stop inside a row).
    void setSnapScrollToRows(bool snap);
    void scrollBy(double rows);
    void scrollToOldest();
    void followTail();
    void advanceViewport(double seconds);
    const ScrollViewport &viewport() const noexcept;
    // GUI-independent frame creation. Advance viewport animation separately.
    // Frame cells are borrowed until the next frame(), mutation, or
    // destruction. Caller may hide the returned cursor for blink timing.
    RenderFrame frame(int cellHeight) const &;
    RenderFrame frame(int cellHeight) const && = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
