#include "copymode.h"
#include "mls-unit-test/unittest.h"

#include <optional>
#include <string>

using namespace frontend;
using Cell = TerminalModel::TextCell;

namespace {
KeyChord ch(int key) {
    return KeyChord{key, false, false, false};
}
KeyChord ctrl(int key) {
    return KeyChord{key, true, false, false};
}
struct Fixture {
    TerminalModel model{20, 3, 20};
    std::optional<Selection> selection;
    std::optional<CopyMode> mode;
    // Three lines on screen after the text below has scrolled once: "one two
    // three" has gone to history.
    Fixture() {
        model.write(
            U"one two three\r\nfour five\r\nsix seven eight\r\nlast line");
        mode.emplace(model);
    }
    CopyMode::Result press(KeyChord chord) {
        return mode->key(chord, model, selection);
    }
    void press(const std::string &keys) {
        for (const char c : keys) {
            press(ch(c));
        }
    }
    std::size_t first() const {
        return model.firstRetainedRow();
    }
};
} // namespace

TEST_SUIT_BEGIN(CopyMode)

TEST_CASE("It starts on the real cursor and moves with hjkl and the arrows, "
          "clamped to the text") {
    Fixture f;
    // "last line" is the last row; the cursor is after it (column 9).
    EXPECT_EQ(f.mode->cursor().row, f.first() + f.model.historySize() + 2);
    EXPECT_EQ(f.mode->cursor().column, std::size_t(9));
    f.press("0");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(0));
    f.press("hh"); // Stays at the left edge.
    EXPECT_EQ(f.mode->cursor().column, std::size_t(0));
    f.press("ll");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(2));
    f.press("k");
    EXPECT_EQ(f.mode->cursor().row, f.first() + f.model.historySize() + 1);
    f.press(ch(keys::Down));
    f.press(ch(keys::Down)); // Past the last row: stays.
    EXPECT_EQ(f.mode->cursor().row, f.first() + f.model.historySize() + 2);
    f.press(ch(keys::Right));
    EXPECT_EQ(f.mode->cursor().column, std::size_t(3));
    f.press(ch(keys::Left));
    EXPECT_EQ(f.mode->cursor().column, std::size_t(2));
}

TEST_CASE("Line keys and top/bottom") {
    Fixture f;
    f.press("k"); // "six seven eight"
    f.press("$");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(14));
    f.press("0");
    f.press("^");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(0));
    f.press(ch(keys::End));
    EXPECT_EQ(f.mode->cursor().column, std::size_t(14));
    f.press(ch(keys::Home));
    EXPECT_EQ(f.mode->cursor().column, std::size_t(0));
    f.press("g");
    EXPECT_EQ(f.mode->cursor().row, f.first());
    f.press("G");
    EXPECT_EQ(f.mode->cursor().row, f.first() + f.model.historySize() + 2);
}

TEST_CASE("Word motions follow words, not cells") {
    Fixture f;
    f.press("g0"); // The oldest line: "one two three"
    f.press("w");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(4)); // "two"
    f.press("w");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(8)); // "three"
    f.press("e");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(12)); // End of "three"
    f.press("b");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(8));
    f.press("b");
    EXPECT_EQ(f.mode->cursor().column, std::size_t(4));
    f.press("w");
    f.press("w"); // Over the row end to the next line's first word.
    EXPECT_EQ(f.mode->cursor().row, f.first() + 1);
    EXPECT_EQ(f.mode->cursor().column, std::size_t(0));
}

TEST_CASE("v selects while moving, y copies it and leaves") {
    Fixture f;
    f.press("g0"); // "one two three"
    f.press("v");
    EXPECT(f.mode->selecting());
    f.press("e"); // End of "one"
    f.press("e"); // End of "two"
    const auto r = f.press(ch('y'));
    EXPECT(r.action == CopyMode::Action::Copy);
    EXPECT_EQ(r.text, "one two");
    EXPECT(!f.selection.has_value());
}

TEST_CASE("Space starts a selection, Enter copies, and v again ends one") {
    Fixture f;
    f.press("g0");
    f.press(ch(' '));
    f.press("ll");
    EXPECT(f.mode->selecting());
    f.press("v"); // Toggles off.
    EXPECT(!f.mode->selecting());
    EXPECT(!f.selection.has_value());
    f.press(ch(' '));
    f.press("l");
    const auto r = f.press(ch(keys::Enter));
    EXPECT(r.action == CopyMode::Action::Copy);
    EXPECT_EQ(r.text, "e "); // Columns 2 and 3 of "one two three".
}

TEST_CASE(
    "V selects the whole line and Enter without a selection yanks the line") {
    Fixture f;
    f.press("g");
    auto r = f.press(ch(keys::Enter));
    EXPECT(r.action == CopyMode::Action::Copy);
    EXPECT(r.text.rfind("one two three", 0) == 0);
    Fixture g;
    g.press("g");
    g.press("V");
    EXPECT(g.mode->selecting());
    g.press("j");
    const auto both = g.press(ch('y'));
    EXPECT(both.text.rfind("one two three", 0) == 0);
    EXPECT(both.text.find("four five") != std::string::npos);
}

TEST_CASE(
    "Escape, q and Ctrl+C leave without copying and clear the selection") {
    for (const auto leaveKey : {ch(keys::Escape), ch('q'), ctrl('c')}) {
        Fixture f;
        f.press("g0v");
        f.press("l");
        const auto r = f.press(leaveKey);
        EXPECT(r.action == CopyMode::Action::Leave);
        EXPECT(r.text.empty());
        EXPECT(!f.selection.has_value());
    }
}

TEST_CASE("Unknown keys are swallowed and change nothing") {
    Fixture f;
    const auto before = f.mode->cursor();
    for (const int key :
         {int('z'), int('1'), int(keys::Tab), int(keys::Backspace)}) {
        const auto r = f.press(ch(key));
        EXPECT(r.action == CopyMode::Action::None);
        EXPECT(!r.changed);
    }
    EXPECT_EQ(f.mode->cursor().row, before.row);
    EXPECT_EQ(f.mode->cursor().column, before.column);
}

TEST_CASE("Pages move by the screen height and the view follows the cursor") {
    TerminalModel model(10, 4, 100);
    for (int i = 0; i < 40; ++i) {
        model.write(U"line\r\n");
    }
    std::optional<Selection> selection;
    CopyMode mode(model);
    const auto start = mode.cursor().row;
    mode.key(ch(keys::PageUp), model, selection);
    EXPECT_EQ(mode.cursor().row, start - 4);
    mode.key(ctrl('u'), model, selection);
    EXPECT_EQ(mode.cursor().row, start - 6);
    for (int i = 0; i < 20; ++i) {
        mode.key(ch(keys::PageUp), model, selection);
    }
    // The view was scrolled to keep the cursor in sight.
    const auto viewTop =
        model.firstRetainedRow() + std::size_t(model.viewport().maxOffset() -
                                               model.viewport().targetOffset());
    EXPECT(mode.cursor().row >= viewTop &&
           mode.cursor().row < viewTop + model.rows());
    mode.key(ch('G'), model, selection);
    model.advanceViewport(10);
    EXPECT(model.viewport().atTail());
    mode.key(ch(keys::PageDown), model, selection);
    EXPECT_EQ(mode.cursor().row,
              model.firstRetainedRow() + model.historySize() + model.rows() -
                  1);
}

TEST_CASE(
    "The virtual cursor is drawn where it is, and not when it is out of view") {
    Fixture f;
    f.press("g0");
    f.model.advanceViewport(10);
    auto frame = f.model.frame(16);
    f.mode->apply(f.model, frame);
    EXPECT(frame.cursor.visible);
    EXPECT(frame.cursor.shape == CursorState::Shape::Block);
    EXPECT(!frame.cursor.blink);
    const auto viewFirst =
        f.model.firstRetainedRow() + f.model.viewport().firstRow();
    EXPECT_EQ(std::size_t(frame.cursor.row), f.mode->cursor().row - viewFirst);
}

TEST_CASE("The badge says COPY or SELECT and where the cursor is") {
    Fixture f;
    f.press("g0");
    EXPECT(f.mode->badge(f.model).find("COPY 1/") != std::string::npos);
    f.press("v");
    EXPECT(f.mode->badge(f.model).find("SELECT 1/") != std::string::npos);
}

TEST_CASE("A selection that became invalid (resize) ends the selecting state "
          "instead of crashing") {
    Fixture f;
    f.press("g0v");
    f.model.resize(15, 3);
    const auto r = f.press(ch('l'));
    EXPECT(!f.mode->selecting());
    EXPECT(!f.selection.has_value());
    EXPECT(r.changed);
}

TEST_CASE("/ searches forward, n and N repeat, ? searches backward, all with "
          "wrap-around") {
    TerminalModel model(20, 3, 20);
    model.write(
        U"one two three\r\nfour five\r\nsix seven eight\r\nlast line two");
    std::optional<Selection> selection;
    CopyMode mode(model);
    const auto first = model.firstRetainedRow();
    auto type = [&](const std::string &text) {
        for (const char c : text) {
            mode.key(ch(c), model, selection);
        }
    };
    mode.key(ch('g'), model, selection); // Row 0 of the text, column 0.
    type("/two");
    EXPECT(mode.badge(model).find("/two") !=
           std::string::npos); // The prompt shows what is typed.
    mode.key(ch(keys::Enter), model, selection);
    // "two" in the first row (column 4) comes after column 0.
    EXPECT_EQ(mode.cursor().row, first);
    EXPECT_EQ(mode.cursor().column, std::size_t(4));
    type("n"); // The next one: "last line two" (row 3, column 10).
    EXPECT_EQ(mode.cursor().row, first + 3);
    EXPECT_EQ(mode.cursor().column, std::size_t(10));
    type("n"); // Wraps to the first again.
    EXPECT_EQ(mode.cursor().row, first);
    EXPECT(mode.badge(model).find("wrapped") != std::string::npos);
    type("N"); // The other direction: back to the last row.
    EXPECT_EQ(mode.cursor().row, first + 3);
    type("?five");
    mode.key(ch(keys::Enter), model, selection);
    EXPECT_EQ(mode.cursor().row, first + 1);
    EXPECT_EQ(mode.cursor().column, std::size_t(5));
}

TEST_CASE("Search: smart case, not found, Esc and Backspace while typing") {
    Fixture f;
    f.press("g0");
    f.press("/ONE"); // A capital: case matters, and "ONE" is not there.
    f.press(ch(keys::Enter));
    EXPECT(f.mode->badge(f.model).find("not found") != std::string::npos);
    EXPECT_EQ(f.mode->cursor().row, f.first());
    f.press("/");
    f.press("tzz");
    f.press(ch(keys::Backspace));
    f.press(ch(keys::Backspace));
    f.press(ch(keys::Escape)); // Cancels the search, not the mode.
    EXPECT(f.mode->badge(f.model).find("COPY") != std::string::npos);
    f.press("/LAST"); // No match with capitals.
    f.press(ch(keys::Enter));
    EXPECT(f.mode->badge(f.model).find("not found") != std::string::npos);
    f.press("/last"); // No capitals: matches "last" case-insensitively.
    f.press(ch(keys::Enter));
    EXPECT_EQ(f.mode->cursor().row, f.first() + f.model.historySize() + 2);
    f.press("n");
    EXPECT(f.mode->badge(f.model).find("wrapped") != std::string::npos);
}

TEST_CASE(
    "A search in a selection extends it, and matches are marked in the frame") {
    Fixture f;
    f.press("g0v");
    f.press("/five");
    f.press(ch(keys::Enter));
    f.model.advanceViewport(10);
    auto frame = f.model.frame(16);
    f.mode->apply(f.model, frame);
    EXPECT(!frame.matchRanges.empty());
    // The typed pattern is marked too while typing.
    f.press("y");
    // y copied from the start of the text to the match.
    Fixture g;
    g.press("g0v/five");
    g.press(ch(keys::Enter));
    const auto r = g.press(ch('y'));
    EXPECT(r.text.rfind("one two three\nfour ", 0) == 0);
}

TEST_CASE("Ctrl+V selects a block; y copies the columns of every row") {
    Fixture f;
    f.press("g0"); // "one two three" row 0, column 0
    f.press(ctrl('v'));
    EXPECT(f.mode->selecting());
    EXPECT(f.mode->badge(f.model).find("BLOCK") != std::string::npos);
    f.press("jjll"); // Rows 0..2, columns 0..2
    f.model.advanceViewport(10);
    auto frame = f.model.frame(16);
    f.mode->apply(f.model, frame);
    EXPECT_EQ(frame.selectionRanges.size(), std::size_t(3));
    const auto r = f.press(ch('y'));
    EXPECT(r.action == CopyMode::Action::Copy);
    EXPECT_EQ(r.text, "one\nfou\nsix");
    EXPECT(!f.mode->selecting());
}

TEST_CASE("Ctrl+V again, or v, ends a block; moving the other way keeps the "
          "rectangle normalized") {
    Fixture f;
    f.press("g");
    f.press("jjj$"); // Last row, last non-blank column: 8
    f.press(ctrl('v'));
    f.press("kkk0"); // Up to row 0, column 0
    const auto r = f.press(ch('y'));
    EXPECT(r.text.rfind("one two th", 0) ==
           std::string::npos); // Block, not the linear text
    EXPECT(r.text.find('\n') != std::string::npos);
    Fixture g;
    g.press(ctrl('v'));
    g.press(ctrl('v'));
    EXPECT(!g.mode->selecting());
}

TEST_SUIT_END
