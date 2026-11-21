#include "mls-unit-test/unittest.h"
#include "textselection.h"

#include <limits>
#include <string>

namespace {
using frontend::selectAt;
using frontend::Selection;
using Cell = TerminalModel::TextCell;

std::string text(const Selection &selection, const TerminalModel &model) {
    return selection.text(model).value_or("<invalid selection>");
}
} // namespace

TEST_SUIT_BEGIN(TextSelection)

TEST_CASE("Shift click preserves the anchor and shift drag continues in either "
          "direction") {
    TerminalModel model(20, 2);
    model.write(U"0123456789");
    std::optional<Selection> selected;
    selectAt(selected, model, {0, 3}, false, 1);
    selectAt(selected, model, {0, 7}, true, 1);
    EXPECT_EQ(selected->anchor.column, 3u);
    EXPECT_EQ(text(*selected, model), "34567");
    selected->extend(model, {0, 9});
    EXPECT_EQ(text(*selected, model), "3456789");
    selectAt(selected, model, {0, 1}, true, 1);
    EXPECT_EQ(selected->anchor.column, 3u);
    EXPECT_EQ(text(*selected, model), "123");
    selected->extend(model, {0, 0});
    EXPECT_EQ(text(*selected, model), "0123");
    selectAt(selected, model, {0, 5}, false, 1);
    EXPECT_EQ(text(*selected, model), "5");
}

TEST_CASE("Shift click without a selection starts a cell selection") {
    TerminalModel model(20, 2);
    model.write(U"hello world");
    std::optional<Selection> selected;
    selectAt(selected, model, {0, 2}, true, 2);
    EXPECT_EQ(text(*selected, model), "l");
    selected->extend(model, {0, 4});
    EXPECT_EQ(text(*selected, model), "llo");
}

TEST_CASE(
    "Double click selects path URL words and whitespace without locale state") {
    TerminalModel model(80, 2);
    model.write(U"one  https://host/a_b-c?q=x&v=2#top%20+~@  two,three");
    auto selected = Selection::at(model, {0, 14}, 2);
    EXPECT_EQ(text(selected, model), "https://host/a_b-c?q=x&v=2#top%20+~@");
    EXPECT_EQ(text(Selection::at(model, {0, 3}, 2), model), "  ");
    EXPECT_EQ(text(Selection::at(model, {0, 46}, 2), model), ",");
    TerminalModel unicode(20, 2);
    unicode.write(U"åäö 世界\u3000next");
    EXPECT_EQ(text(Selection::at(unicode, {0, 1}, 2), unicode), "åäö");
    EXPECT_EQ(text(Selection::at(unicode, {0, 4}, 2), unicode), "世界");
}

TEST_CASE("Words cross only soft wrapped row boundaries") {
    TerminalModel model(6, 4);
    model.write(U"xx foobar!");
    auto selected = Selection::at(model, {1, 1}, 2);
    EXPECT_EQ(selected.anchor.row, 0u);
    EXPECT_EQ(selected.anchor.column, 3u);
    EXPECT_EQ(selected.end.row, 1u);
    EXPECT_EQ(selected.end.column, 2u);
    EXPECT_EQ(text(selected, model), "foobar");
    EXPECT(model.textRowWrapped(0));
    EXPECT(!model.textRowWrapped(1));
    EXPECT_EQ(*model.textCodePoint({1, 0}), U'b');
    EXPECT(!model.textCodePoint({0, 6}));
    EXPECT(!model.textCodePoint({4, 0}));
    EXPECT(!model.textRowWrapped(std::numeric_limits<std::size_t>::max()));

    TerminalModel hard(3, 3);
    hard.write(U"abc\r\ndef");
    EXPECT_EQ(text(Selection::at(hard, {1, 0}, 2), hard), "def");
    EXPECT_EQ(text(Selection::at(hard, {0, 2}, 2), hard), "abc");
}

TEST_CASE(
    "Word drag keeps complete starting and destination words when reversing") {
    TerminalModel model(30, 2);
    model.write(U"one two three four");
    auto selected = Selection::at(model, {0, 5}, 2);
    selected.extend(model, {0, 10});
    EXPECT_EQ(text(selected, model), "two three");
    selected.extend(model, {0, 0});
    EXPECT_EQ(text(selected, model), "one two");
    selected.extend(model, {0, 16});
    EXPECT_EQ(text(selected, model), "two three four");
    selected.shiftTo({0, 8});
    selected.extend(model, {0, 9});
    EXPECT_EQ(text(selected, model), "two th");
}

TEST_CASE(
    "Triple click selects a logical line and drag extends by complete lines") {
    TerminalModel model(5, 5);
    model.write(U"first\r\nabcdefghi\r\nlast");
    auto selected = Selection::at(model, {2, 1}, 3);
    EXPECT_EQ(selected.anchor.row, 1u);
    EXPECT_EQ(selected.anchor.column, 0u);
    EXPECT_EQ(selected.end.row, 2u);
    EXPECT_EQ(selected.end.column, 4u);
    EXPECT_EQ(text(selected, model), "abcdefghi ");
    selected.extend(model, {3, 1});
    EXPECT_EQ(text(selected, model), "abcdefghi\nlast ");
    selected.extend(model, {0, 4});
    EXPECT_EQ(text(selected, model), "first\nabcdefghi ");
    selected.extend(model, {2, 0});
    EXPECT_EQ(text(selected, model), "abcdefghi ");
}

TEST_CASE(
    "Select all spans retained history and the screen regardless of viewport") {
    TerminalModel model(4, 2, 10);
    model.write(U"zero\r\none\r\ntwo\r\nlast");
    model.scrollToOldest();
    model.advanceViewport(1);
    const auto selected = Selection::all(model);
    EXPECT_EQ(selected.anchor.row, model.firstRetainedRow());
    EXPECT_EQ(selected.end.row,
              model.firstRetainedRow() + model.historySize() + 1);
    EXPECT_EQ(text(selected, model), "zero\none\ntwo\nlast");
    std::optional<Selection> extended;
    selectAt(extended, model, selected.anchor, false, 1);
    selectAt(extended, model, selected.end, true, 1);
    EXPECT_EQ(text(*extended, model), text(selected, model));
    auto frame = model.frame(10);
    selected.apply(model, frame);
    EXPECT_EQ(frame.selectionBegin, 0u);
    EXPECT_EQ(frame.selectionEnd, 8u);
    model.followTail();
    model.advanceViewport(10);
    frame = model.frame(10);
    selected.apply(model, frame);
    EXPECT_EQ(frame.selectionBegin, 0u);
    EXPECT_EQ(frame.selectionEnd, 8u);
    EXPECT(selected.valid(model));
}

TEST_CASE("Word and line bounds work in scrollback and stop at evicted rows") {
    TerminalModel model(4, 2, 2);
    model.write(U"123456789abcdef\r\nnext\r\nlast");
    const auto first = model.firstRetainedRow();
    EXPECT_GT(first, 0u);
    EXPECT(!model.textCodePoint({first - 1, 0}));
    const auto word = Selection::at(model, {first, 1}, 2);
    EXPECT_EQ(word.anchor.row, first);
    EXPECT_EQ(text(word, model), "9abcdef");
    const auto line = Selection::at(model, {first, 1}, 3);
    EXPECT_EQ(text(line, model), "9abcdef ");
    EXPECT_EQ(text(Selection::all(model), model), "9abcdef\nnext\nlast");
}

TEST_CASE("Epoch changes invalidate copy highlighting and Shift anchors") {
    TerminalModel model(10, 3, 2);
    model.write(U"hello");
    std::optional<Selection> selected = Selection::at(model, {0, 1}, 1);
    model.resize(12, 3);
    EXPECT(!selected->valid(model));
    EXPECT(!selected->text(model));
    auto frame = model.frame(10);
    const auto begin = frame.selectionBegin, end = frame.selectionEnd;
    selected->apply(model, frame);
    EXPECT_EQ(frame.selectionBegin, begin);
    EXPECT_EQ(frame.selectionEnd, end);
    selectAt(selected, model, {0, 4}, true, 1);
    EXPECT_EQ(selected->anchor.column, 4u);
    EXPECT(selected->valid(model));
    model.setAlternateScreen(true);
    EXPECT(!selected->valid(model));
    selected = Selection::all(model);
    EXPECT(selected->valid(model));
    EXPECT_EQ(selected->end.row - selected->anchor.row, model.rows() - 1);
    model.reset();
    EXPECT(!selected->valid(model));
    selected = Selection::all(model);
    model.moveCursor(0, 1);
    model.insertLines(1);
    EXPECT(!selected->valid(model));
}

TEST_CASE("Eviction and invalid endpoints cannot be copied or extended") {
    TerminalModel model(3, 2, 1);
    model.write(U"aaa\r\nbbb");
    auto selected = Selection::all(model);
    model.write(U"\r\nccc\r\nddd");
    EXPECT(!selected.valid(model));
    EXPECT(!selected.text(model));
    const auto oldEnd = selected.end;
    selected.extend(model, {model.firstRetainedRow(), 0});
    EXPECT_EQ(selected.end.row, oldEnd.row);
    selected = Selection::all(model);
    selected.end.column = model.columns();
    EXPECT(!selected.valid(model));
    selected = Selection::all(model);
    selected.end.row += 1;
    EXPECT(!selected.text(model));
}

TEST_CASE("Select all has an explicit bounded copy rather than truncating "
          "large content") {
    TerminalModel model(1024, 2, 1024);
    model.write(std::u32string(1024 * 1025, U'x'));
    auto selected = Selection::all(model);
    EXPECT(selected.valid(model));
    EXPECT(!selected.text(model));
    const auto complete = selected.text(model, 1024 * 1025);
    EXPECT(bool(complete));
    EXPECT_EQ(complete->size(), 1024u * 1025u);
    selected.end = {selected.anchor.row, 3};
    EXPECT_EQ(selected.text(model, 4).value_or("error"), "xxxx");
    EXPECT(!selected.text(model, 3));
}

TEST_CASE("A single-cell character selection is not highlighted") {
    TerminalModel model(10, 3, 2);
    model.write(U"hello");
    const auto one = Selection::at(model, {0, 1}, 1);
    auto frame = model.frame(10);
    one.apply(model, frame);
    EXPECT_EQ(frame.selectionBegin, 0u);
    EXPECT_EQ(frame.selectionEnd, 0u);
    EXPECT_EQ(text(one, model), "e");
    const auto word = Selection::at(model, {0, 1}, 2);
    word.apply(model, frame);
    EXPECT_EQ(frame.selectionEnd, 5u);
}

TEST_SUIT_END
