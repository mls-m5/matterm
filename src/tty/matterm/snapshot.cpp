#include "tty/matterm/snapshot.h"

#include "tty/utf8.h"

namespace tty {
namespace {

struct Pen {
    CellAttribute style = CellAttribute::None;
    ColorReference foreground = ColorReference::defaultColor();
    ColorReference background = ColorReference::defaultColor();
    bool plain() const {
        return style == CellAttribute::None &&
               foreground.kind == ColorReference::Kind::Default &&
               background.kind == ColorReference::Kind::Default;
    }
};

bool sameColor(const ColorReference &a, const ColorReference &b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case ColorReference::Kind::Default:
        return true;
    case ColorReference::Kind::Indexed:
        return a.value.r == b.value.r;
    case ColorReference::Kind::Rgb:
        return a.value.r == b.value.r && a.value.g == b.value.g &&
               a.value.b == b.value.b;
    }
    return false;
}

Pen penOf(const FChar &cell) {
    Pen pen;
    pen.style = cell.style();
    pen.foreground = cell.foregroundColor();
    pen.background = cell.backgroundColor();
    return pen;
}

bool samePen(const Pen &a, const Pen &b) {
    return a.style == b.style && sameColor(a.foreground, b.foreground) &&
           sameColor(a.background, b.background);
}

void appendColor(std::string &out,
                 const ColorReference &color,
                 bool foreground) {
    switch (color.kind) {
    case ColorReference::Kind::Default:
        out += foreground ? "39" : "49";
        break;
    case ColorReference::Kind::Indexed: {
        const int n = color.value.r;
        if (n < 8) {
            out += std::to_string((foreground ? 30 : 40) + n);
        }
        else if (n < 16) {
            out += std::to_string((foreground ? 90 : 100) + n - 8);
        }
        else {
            out +=
                std::string(foreground ? "38;5;" : "48;5;") + std::to_string(n);
        }
        break;
    }
    case ColorReference::Kind::Rgb:
        out += std::string(foreground ? "38;2;" : "48;2;") +
               std::to_string(color.value.r) + ";" +
               std::to_string(color.value.g) + ";" +
               std::to_string(color.value.b);
        break;
    }
}

// Sets the host's pen to `wanted` (a full state: reset, then everything that is
// not the default).
void setPen(std::string &out, Pen &pen, const Pen &wanted) {
    if (samePen(pen, wanted)) {
        return;
    }
    out += "\x1b[0";
    const struct {
        CellAttribute flag;
        const char *code;
    } codes[] = {{CellAttribute::Bold, ";1"},
                 {CellAttribute::Faint, ";2"},
                 {CellAttribute::Italic, ";3"},
                 {CellAttribute::Underline, ";4"},
                 {CellAttribute::Blink, ";5"},
                 {CellAttribute::Inverse, ";7"},
                 {CellAttribute::Conceal, ";8"},
                 {CellAttribute::Strike, ";9"}};
    for (const auto &c : codes) {
        if (hasAttribute(wanted.style, c.flag)) {
            out += c.code;
        }
    }
    if (wanted.foreground.kind != ColorReference::Kind::Default) {
        out += ';';
        appendColor(out, wanted.foreground, true);
    }
    if (wanted.background.kind != ColorReference::Kind::Default) {
        out += ';';
        appendColor(out, wanted.background, false);
    }
    out += 'm';
    pen = wanted;
}

bool plainBlank(const FChar &cell) {
    return (cell.codePoint == U' ' || cell.codePoint == 0) &&
           penOf(cell).plain();
}

// The cells of one row up to the last one that is not a plain blank.
// `continuation`: the row before ran on into this one; the host only moves to
// the next row when a character arrives, so a blank continuation still needs
// one (a space).
template <class At>
void writeRow(std::string &out,
              Pen &pen,
              std::size_t columns,
              const At &at,
              bool continuation = false) {
    std::size_t end = columns;
    while (end > 0 && plainBlank(at(end - 1))) {
        --end;
    }
    if (continuation && end == 0) {
        end = 1; // The blank first cell is the character that moves the host to
                 // this row.
    }
    for (std::size_t x = 0; x < end; ++x) {
        const FChar &cell = at(x);
        setPen(out, pen, penOf(cell));
        appendUtf8(out, cell.codePoint < 0x20 ? U' ' : cell.codePoint);
    }
    if (end < columns) {
        // The host may show something older here: clear the rest of the row
        // (with the default pen, so that no background colour is spread).
        setPen(out, pen, Pen{});
        out += "\x1b[K";
    }
}

void endLine(std::string &out, Pen &pen) {
    setPen(out,
           pen,
           Pen{}); // A background colour must not spread into the new line.
    out += "\r\n";
}

const char *mouseCode(TerminalModes::MouseTracking tracking) {
    switch (tracking) {
    case TerminalModes::MouseTracking::Press:
        return "9";
    case TerminalModes::MouseTracking::Normal:
        return "1000";
    case TerminalModes::MouseTracking::Drag:
        return "1002";
    case TerminalModes::MouseTracking::Any:
        return "1003";
    case TerminalModes::MouseTracking::Off:
        break;
    }
    return nullptr;
}

} // namespace

std::string snapshot(const TerminalModel &model,
                     const TerminalModes &modes,
                     const SnapshotOptions &options) {
    std::string out;
    Pen pen;
    const std::size_t columns = model.columns(), rows = model.rows();
    // A clean start: no pen, no margins, origin off, autowrap on, cursor hidden
    // while it is drawn.
    out += "\x1b[0m\x1b[?25l\x1b[r\x1b[?6l\x1b[?7h";
    if (model.alternateScreen()) {
        out += "\x1b[?1049h"; // Enters, clears and saves the cursor.
        for (std::size_t y = 0; y < rows; ++y) {
            out += "\x1b[" + std::to_string(y + 1) + ";1H";
            writeRow(out, pen, columns, [&](std::size_t x) -> const FChar & {
                return model.cell(x, y);
            });
            setPen(out, pen, Pen{});
        }
    }
    else {
        if (options.scrollback) {
            const std::size_t saved = model.historySize();
            const std::size_t first = saved > options.maxScrollbackRows
                                          ? saved - options.maxScrollbackRows
                                          : 0;
            for (std::size_t i = first; i < saved; ++i) {
                writeRow(
                    out,
                    pen,
                    columns,
                    [&](std::size_t x) -> const FChar & {
                        return model.historyCell(x, i);
                    },
                    i > first && model.historyRowWrapped(i - 1));
                if (!model.historyRowWrapped(i)) {
                    endLine(out, pen);
                }
            }
        }
        // Exactly `rows` lines: whatever the host showed, the visible area ends
        // up equal to the screen.
        const std::size_t base = model.firstRetainedRow() + model.historySize();
        bool previousWrapped =
            options.scrollback && model.historySize() > 0 &&
            model.historyRowWrapped(model.historySize() - 1) &&
            options.maxScrollbackRows > 0;
        for (std::size_t y = 0; y < rows; ++y) {
            writeRow(
                out,
                pen,
                columns,
                [&](std::size_t x) -> const FChar & {
                    return model.cell(x, y);
                },
                previousWrapped);
            previousWrapped = model.textRowWrapped(base + y);
            if (y + 1 < rows && !previousWrapped) {
                endLine(out, pen);
            }
        }
        setPen(out, pen, Pen{});
    }
    // Margins, origin mode, then the cursor (relative to the margins when
    // origin mode is on).
    const int top = model.scrollTop(), bottom = model.scrollBottom();
    if (top != 0 || bottom != int(rows) - 1) {
        out += "\x1b[" + std::to_string(top + 1) + ";" +
               std::to_string(bottom + 1) + "r";
    }
    if (model.originMode()) {
        out += "\x1b[?6h";
    }
    const auto cursor = model.cursor();
    const int row = cursor.row - (model.originMode() ? top : 0);
    out += "\x1b[" + std::to_string(row + 1) + ";" +
           std::to_string(cursor.column + 1) + "H";
    if (cursor.shape != CursorState::Shape::Block ||
        !cursor.blink) { // The model's (and the host's) default: a blinking
                         // block.
        const int base = cursor.shape == CursorState::Shape::Block       ? 1
                         : cursor.shape == CursorState::Shape::Underline ? 3
                                                                         : 5;
        out += "\x1b[" + std::to_string(base + (cursor.blink ? 0 : 1)) + " q";
    }
    // What the program asked of the host's input.
    if (modes.applicationCursor) {
        out += "\x1b[?1h";
    }
    if (modes.applicationKeypad) {
        out += "\x1b=";
    }
    if (modes.bracketedPaste) {
        out += "\x1b[?2004h";
    }
    if (modes.focusReporting) {
        out += "\x1b[?1004h";
    }
    if (const char *code = mouseCode(modes.mouseTracking)) {
        out += std::string("\x1b[?") + code + "h";
        switch (modes.mouseEncoding) {
        case TerminalModes::MouseEncoding::X10:
            break;
        case TerminalModes::MouseEncoding::Utf8:
            out += "\x1b[?1005h";
            break;
        case TerminalModes::MouseEncoding::Sgr:
            out += "\x1b[?1006h";
            break;
        case TerminalModes::MouseEncoding::Urxvt:
            out += "\x1b[?1015h";
            break;
        }
    }
    if (cursor.visible) {
        out += "\x1b[?25h";
    }
    return out;
}

std::string hostRestore(bool inAlternateScreen) {
    std::string out =
        "\x1b[?1004l\x1b[?2004l\x1b[?1006l\x1b[?1015l\x1b[?1005l\x1b[?"
        "1003l\x1b[?1002l\x1b[?1000l\x1b[?9l"
        "\x1b[?1l\x1b>\x1b[r\x1b[?6l\x1b(B\x1b[0m\x1b[0 q\x1b[?25h";
    if (inAlternateScreen) {
        out += "\x1b[?1049l";
    }
    out += "\r\n";
    return out;
}

} // namespace tty
