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

TEST_SUIT_END
