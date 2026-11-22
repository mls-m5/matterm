// Reflow of saved rows on a width change (docs/SCROLLING_AND_REFLOW.md): the layout algorithm on
// its own, the model around it (flags, padding rule, cursor, viewport, alternate screen, capacity)
// and the lazy part (newest rows at once, older ones in slices).
#include "mls-unit-test/unittest.h"
#include "reflow.h"
#include "terminalmodel.h"

#include <random>
#include <string>
#include <vector>

namespace {
struct Row {
    std::u32string text; // As sunk: no padding.
    bool wraps;
};
std::vector<Row> lay(const std::vector<std::pair<std::u32string, bool>> &input, std::size_t width,
                     std::size_t anchor = static_cast<std::size_t>(-1), reflow::Anchor *out = nullptr) {
    // Input rows are padded to their width with spaces like stored rows.
    std::vector<std::vector<FChar>> rows;
    for (const auto &[text, wraps] : input) {
        std::vector<FChar> r;
        for (const char32_t c : text) {
            FChar cell;
            cell.codePoint = c;
            r.push_back(cell);
        }
        rows.push_back(std::move(r));
    }
    std::vector<Row> result;
    reflow::Anchor a;
    a.row = anchor;
    reflow::layout(
        0, rows.size(), width,
        [&](std::size_t i) { return FStringView(rows[i].data(), rows[i].size()); },
        [&](std::size_t i) { return input[i].second; },
        [&](const FChar *cells, std::size_t n, bool wraps) {
            std::u32string t;
            for (std::size_t i = 0; i < n; ++i) t.push_back(cells[i].codePoint);
            result.push_back({t, wraps});
        },
        [](const FChar &c) { return c.codePoint == U' '; }, &a, 1);
    if (out) *out = a;
    return result;
}

std::string canon(TerminalModel &m, bool includeLive = false) {
    const std::size_t rows = m.historySize() + (includeLive ? m.rows() : 0);
    if (rows == 0) return {};
    const auto first = m.firstRetainedRow();
    const auto text = m.copyText({first, 0}, {first + rows - 1, m.columns() - 1}, 1u << 26);
    std::string out, line;
    for (const char c : text.value_or("")) {
        if (c == '\n') {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line + "\n";
            line.clear();
        }
        else line += c;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    return out + line;
}

// Writes `lines` (each a hard line) followed by enough output to put them all in the history.
void fill(TerminalModel &m, const std::vector<std::u32string> &lines) {
    for (const auto &l : lines) {
        m.write(l);
        m.carriageReturn();
        m.lineFeed();
    }
    for (std::size_t i = 0; i < m.rows(); ++i) m.lineFeed();
}

std::vector<std::u32string> sample(std::size_t count, std::size_t maxLength, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::u32string> lines;
    for (std::size_t i = 0; i < count; ++i) {
        std::u32string l;
        const std::size_t n = rng() % 5 == 0 ? 0 : 1 + rng() % maxLength;
        for (std::size_t k = 0; k < n; ++k) l += rng() % 9 == 0 ? U' ' : char32_t(U'a' + rng() % 26);
        while (!l.empty() && l.back() == U' ') l.pop_back(); // The padding rule drops these anyway.
        l += char32_t(U'0' + i % 10);                         // A marker so no line ends in a blank.
        lines.push_back(l);
    }
    return lines;
}
std::string join(const std::vector<std::u32string> &lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        for (const char32_t c : lines[i]) out += char(c);
        if (i + 1 < lines.size()) out += "\n";
    }
    return out;
}
} // namespace

TEST_SUIT_BEGIN(TerminalReflow)

TEST_CASE("layout: joins soft wraps, cuts at the new width, drops trailing padding") {
    // "abcdef" wrapped at 3, then a hard line "gh".
    const auto rows = lay({{U"abc", true}, {U"def", false}, {U"gh ", false}}, 4);
    EXPECT_EQ(rows.size(), std::size_t(3));
    EXPECT(rows[0].text == U"abcd" && rows[0].wraps);
    EXPECT(rows[1].text == U"ef" && !rows[1].wraps);
    EXPECT(rows[2].text == U"gh" && !rows[2].wraps); // Trailing blank is padding.
    // Wider: one row.
    const auto wide = lay({{U"abc", true}, {U"def", false}}, 20);
    EXPECT_EQ(wide.size(), std::size_t(1));
    EXPECT(wide[0].text == U"abcdef" && !wide[0].wraps);
}

TEST_CASE("layout: an exactly full line is one row, empty lines stay, inner blanks stay") {
    const auto exact = lay({{U"abcd", false}}, 4);
    EXPECT_EQ(exact.size(), std::size_t(1));
    EXPECT(!exact[0].wraps);
    const auto blank = lay({{U"", false}, {U"   ", false}, {U"x", false}}, 5);
    EXPECT_EQ(blank.size(), std::size_t(3));
    EXPECT(blank[0].text.empty() && blank[1].text.empty() && blank[2].text == U"x");
    const auto inner = lay({{U"a  b", false}}, 10);
    EXPECT(inner[0].text == U"a  b");
    // A line of padding only inside a wrapped line, then text: kept.
    const auto joined = lay({{U"ab", true}, {U"  ", true}, {U"cd", false}}, 3);
    EXPECT_EQ(joined.size(), std::size_t(2));
    EXPECT(joined[0].text == U"ab " && joined[0].wraps);
    EXPECT(joined[1].text == U" cd");
}

TEST_CASE("layout: padding after an exactly full row ends the line there") {
    const auto rows = lay({{U"abcd  ", false}}, 4);
    EXPECT_EQ(rows.size(), std::size_t(1));
    EXPECT(!rows[0].wraps);
}

TEST_CASE("layout: the last row never wraps, and a trailing full row is not marked as wrapping") {
    // The last row of the range wraps in the input (the line goes on outside): it ends here.
    const auto rows = lay({{U"abc", true}}, 2);
    EXPECT_EQ(rows.size(), std::size_t(2));
    EXPECT(rows[0].wraps && !rows[1].wraps);
    // A wrapped line whose last row is only padding.
    const auto pad = lay({{U"abcd", true}, {U"    ", false}}, 4);
    EXPECT_EQ(pad.size(), std::size_t(1));
    EXPECT(!pad[0].wraps);
}

TEST_CASE("layout: anchors map an input row to its place in the new rows") {
    reflow::Anchor r;
    // Rows 0..2 form one line of 12 cells at width 4; row 2 starts at cell 8: new row 2 at width 4.
    lay({{U"aaaa", true}, {U"bbbb", true}, {U"cccc", false}, {U"dd", false}}, 4, 2, &r);
    EXPECT(r.found);
    EXPECT_EQ(r.outRow, std::size_t(2));
    lay({{U"aaaa", true}, {U"bbbb", true}, {U"cccc", false}, {U"dd", false}}, 6, 2, &r);
    EXPECT_EQ(r.outRow, std::size_t(1)); // Cell 8 is in the second row of six.
    lay({{U"aaaa", true}, {U"bbbb", true}, {U"cccc", false}, {U"dd", false}}, 6, 3, &r);
    EXPECT_EQ(r.outRow, std::size_t(2)); // Next line starts after two rows.
    lay({{U"a", false}}, 4, 5, &r);
    EXPECT(!r.found);
}

TEST_CASE("layout: memory stays bounded for a very long line") {
    std::vector<std::pair<std::u32string, bool>> input;
    for (int i = 0; i < 5000; ++i) input.push_back({std::u32string(80, U'x'), i + 1 < 5000});
    const auto rows = lay(input, 17);
    std::size_t cells = 0;
    for (const auto &r : rows) cells += r.text.size();
    EXPECT_EQ(cells, std::size_t(5000 * 80));
    EXPECT_EQ(rows.size(), std::size_t((5000 * 80 + 16) / 17));
}

TEST_CASE("Narrowing joins nothing it should not, widening restores the lines exactly") {
    for (const unsigned seed : {1u, 2u, 3u, 4u, 5u}) {
        TerminalModel m(40, 8, 500);
        const auto lines = sample(60, 110, seed);
        fill(m, lines);
        const auto original = canon(m);
        for (const std::size_t w : {7u, 13u, 40u, 90u, 40u}) {
            m.resize(w, 8);
            m.finishReflow();
            EXPECT_EQ(canon(m), original);
        }
    }
}

TEST_CASE("The text of the history is the same at every width") {
    TerminalModel m(30, 6, 2000);
    const auto lines = sample(80, 100, 7);
    fill(m, lines);
    const auto before = canon(m);
    EXPECT(before.size() > 100);
    for (std::size_t w = 29; w >= 2; --w) {
        m.resize(w, 6);
        m.finishReflow();
        EXPECT_EQ(canon(m), before);
    }
    for (std::size_t w = 3; w <= 120; w += 7) {
        m.resize(w, 6);
        m.finishReflow();
        EXPECT_EQ(canon(m), before);
    }
}

TEST_CASE("Soft-wrap flags: autowrap sets them, new lines, erases and scrolls do not leave stale ones") {
    TerminalModel m(5, 4, 100);
    m.write(U"abcdefgh"); // Wraps once: row 0 continues.
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 0));
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 1));
    m.carriageReturn();
    m.lineFeed();
    m.write(U"xy");
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 2));
    // Erasing the whole row clears its flag; erasing part of it does not touch it.
    m.moveCursor(0, 0);
    m.eraseCharacters(2);
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 0));
    m.eraseLine(TerminalModel::EraseMode::All);
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 0));
    // Insert / delete characters keep the flag of the row they edit (the line still goes on).
    TerminalModel n(5, 3, 100);
    n.write(U"abcdefg");
    n.moveCursor(1, 0);
    n.insertCharacters(1);
    EXPECT(n.textRowWrapped(n.firstRetainedRow() + n.historySize()));
    // Anything that blanks the end of the row ends the line there (DCH, EL, ECH to the edge).
    n.deleteCharacters(1);
    EXPECT(!n.textRowWrapped(n.firstRetainedRow() + n.historySize()));
    // A cursor move after the wrap was pending cancels it: no flag.
    TerminalModel p(5, 3, 100);
    p.write(U"abcde");
    p.moveCursor(0, 1);
    p.write(U"f");
    EXPECT(!p.textRowWrapped(p.firstRetainedRow() + p.historySize()));
    // A row that scrolls into the history keeps its flag, and a full scroll of the screen does not set one.
    TerminalModel q(4, 2, 100);
    q.write(U"abcdefgh"); // Rows: "abcd" (wraps), "efgh" (deferred wrap pending).
    q.write(U"i");        // Scrolls: "abcd" is history, still wrapping into "efgh".
    EXPECT(q.historySize() == 1);
    EXPECT(q.textRowWrapped(q.firstRetainedRow()));
    q.carriageReturn();
    q.lineFeed();
    q.lineFeed();
    EXPECT(q.textRowWrapped(q.firstRetainedRow() + 1));  // "efgh" goes on in "i".
    EXPECT(!q.textRowWrapped(q.firstRetainedRow() + 2)); // "i" ended with a line feed.
}

TEST_CASE("Soft-wrap flags with scroll regions and line insertion") {
    TerminalModel m(4, 5, 50);
    EXPECT(m.setScrollRegion(1, 3));
    m.moveCursor(0, 1);
    m.write(U"abcdef"); // Row 1 wraps into row 2.
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 1));
    m.moveCursor(0, 1);
    m.insertLines(1); // The wrapped pair moves down: the flag moves with the row, row 1 is new and plain.
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 1));
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 2));
    m.deleteLines(1);
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 1));
    // Rows next to the region edge never keep a flag that points out of it.
    m.moveCursor(0, 3);
    m.write(U"wxyz");
    m.write(U"q"); // Wraps at the bottom of the region: scrolls inside it.
    for (std::size_t y = 0; y < 5; ++y) {
        // The last row of the region and the rows around it must not claim a continuation across the edge.
        if (y == 3 || y == 0) EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + y) || y == 3);
    }
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + m.historySize() + 4));
}

TEST_CASE("Colored blanks at the end of a line are text, default ones are padding") {
    TerminalModel m(10, 3, 100);
    m.setColors({255, 255, 255}, {0, 0, 200});
    m.write(U"ab    "); // Four blue blanks.
    m.setColors({255, 255, 255}, {0, 0, 0});
    m.write(U"cd");
    m.carriageReturn();
    m.lineFeed();
    m.write(U"ef  "); // Two black blanks typed: padding.
    m.carriageReturn();
    m.lineFeed();
    for (int i = 0; i < 4; ++i) m.lineFeed();
    m.resize(4, 3);
    m.finishReflow();
    // Rows of "ab    cd": "ab  " + "  cd"; second line "ef".
    const auto first = m.firstRetainedRow();
    EXPECT(m.textCodePoint({first, 0}) == std::optional<char32_t>(U'a'));
    EXPECT(m.textRowWrapped(first));
    EXPECT(m.textCodePoint({first + 1, 2}) == std::optional<char32_t>(U'c'));
    EXPECT(!m.textRowWrapped(first + 1));
    EXPECT(m.textCodePoint({first + 2, 0}) == std::optional<char32_t>(U'e'));
    EXPECT(!m.textRowWrapped(first + 2));
    EXPECT(m.historySize() >= 3);
}

TEST_CASE("Styles survive a reflow cell by cell") {
    TerminalModel m(8, 3, 100);
    m.setColors({10, 20, 30}, {40, 50, 60});
    m.setAttributes(CellAttribute::Bold);
    m.write(U"abcdef");
    m.setAttributes(CellAttribute::None);
    m.setColors({200, 100, 50}, {0, 0, 0});
    m.write(U"ghij");
    m.carriageReturn();
    m.lineFeed();
    for (int i = 0; i < 4; ++i) m.lineFeed();
    m.resize(3, 3);
    m.finishReflow();
    // "abcdefghij" -> abc|def|ghi|j.
    const auto row = [&](std::size_t y) { return m.firstRetainedRow() + y; };
    EXPECT(m.textCodePoint({row(1), 0}) == std::optional<char32_t>(U'd'));
    // Reach the cells through the frame of the oldest rows.
    m.scrollToOldest();
    m.advanceViewport(10);
    const auto frame = m.frame(16);
    EXPECT(frame.cells.row(0).data()[0].foreground.r == 10);
    EXPECT(frame.cells.row(0).data()[0].background.b == 60);
    EXPECT(hasAttribute(frame.cells.row(0).data()[0].attributes, CellAttribute::Bold));
    EXPECT(frame.cells.row(2).data()[0].foreground.r == 200);
    EXPECT(!hasAttribute(frame.cells.row(2).data()[0].attributes, CellAttribute::Bold));
}

TEST_CASE("With live reflow off, the live screen clips and pads; the alternate screen always does") {
    TerminalModel m(10, 3, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::Off);
    m.write(U"0123456789"); // Full live row, deferred wrap.
    m.carriageReturn();
    m.lineFeed();
    m.write(U"abcdefghij");
    m.resize(4, 3);
    EXPECT_EQ(m.historySize(), std::size_t(0));
    EXPECT(m.cell(3, 0).codePoint == U'3');
    EXPECT(m.cell(3, 1).codePoint == U'd');
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.setAlternateScreen(true);
    m.write(U"ALTE");
    m.resize(6, 3);
    EXPECT(m.cell(3, 0).codePoint == U'E');
    EXPECT(m.cell(4, 0).codePoint == U' ');
    m.resize(2, 3);
    EXPECT(m.cell(1, 0).codePoint == U'L');
    EXPECT_EQ(m.historySize(), std::size_t(0));
}

TEST_CASE("Resizing while the alternate screen is active lays out the primary history") {
    TerminalModel m(20, 4, 200);
    const auto lines = sample(30, 60, 11);
    fill(m, lines);
    const auto before = canon(m);
    m.setAlternateScreen(true);
    m.write(U"vim");
    m.resize(9, 4);
    m.finishReflow();
    m.setAlternateScreen(false);
    EXPECT_EQ(canon(m), before);
}

TEST_CASE("Live rows: wrapped rows are one line, they follow the width both ways") {
    TerminalModel m(6, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"abcdefghi"); // abcdef|ghi
    EXPECT(m.textRowWrapped(m.firstRetainedRow()));
    m.resize(4, 4); // abcd|efgh|i
    EXPECT(m.textRowWrapped(m.firstRetainedRow()));
    EXPECT(m.textRowWrapped(m.firstRetainedRow() + 1));
    EXPECT(!m.textRowWrapped(m.firstRetainedRow() + 2));
    EXPECT_EQ(m.cursor().row, 2);
    EXPECT_EQ(m.cursor().column, 1);
    m.resize(9, 4); // One row, the cursor behind the i; the row is full: the cursor is pending.
    EXPECT(!m.textRowWrapped(m.firstRetainedRow()));
    EXPECT_EQ(m.cursor().row, 0);
    EXPECT_EQ(m.cursor().column, 8);
    m.write(U"j"); // Written behind the i: the line goes on in the next row.
    EXPECT(m.textRowWrapped(m.firstRetainedRow()));
    EXPECT(m.cell(0, 1).codePoint == U'j');
}

TEST_CASE("Live rows: the cursor keeps its place in its line") {
    TerminalModel m(10, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"0123456789ABCDEF"); // 0123456789|ABCDEF, cursor behind F: row 1, column 6.
    m.moveCursor(2, 1);          // On the C.
    m.resize(4, 4);              // 0123|4567|89AB|CDEF: the C starts the fourth row.
    EXPECT_EQ(m.cursor().row, 3);
    EXPECT_EQ(m.cursor().column, 0);
    EXPECT(m.cell(m.cursor().column, std::size_t(m.cursor().row)).codePoint == U'C');
    m.resize(7, 4);
    EXPECT(m.cell(m.cursor().column, std::size_t(m.cursor().row)).codePoint == U'C');
    m.resize(30, 4);
    EXPECT_EQ(m.cursor().row, 0);
    EXPECT_EQ(m.cursor().column, 12);
}

TEST_CASE("Live rows: a cursor behind the text of a prompt line stays behind it") {
    TerminalModel m(20, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"user@host:~$ "); // The blank at the end is padding to the rule, the cursor is behind it.
    EXPECT_EQ(m.cursor().column, 13);
    m.resize(8, 4);
    // "user@hos|t:~$ " : the cursor is behind the blank, in the second row.
    EXPECT_EQ(m.cursor().row, 1);
    EXPECT_EQ(m.cursor().column, 5);
    m.write(U"ls");
    EXPECT(m.cell(5, 1).codePoint == U'l');
    m.resize(30, 4);
    EXPECT_EQ(m.cursor().row, 0);
    EXPECT(m.cell(13, 0).codePoint == U'l');
    EXPECT_EQ(m.cursor().column, 15);
}

TEST_CASE("Live rows: a cursor far out on an empty row does not make rows") {
    TerminalModel m(20, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.moveCursor(15, 2);
    m.resize(8, 4);
    EXPECT_EQ(m.historySize(), std::size_t(0));
    EXPECT_EQ(m.cursor().row, 2);
    EXPECT(m.cursor().column <= 7);
}

TEST_CASE("Live rows: rows that no longer fit go to the history, saved rows come back") {
    TerminalModel m(10, 3, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"0123456789ABCDE\r\nxy\r\nz"); // 3 rows of text, the first line wraps: 4 rows.
    EXPECT_EQ(m.historySize(), std::size_t(1));
    m.resize(5, 3); // 01234|56789|ABCDE|xy|z : 5 rows, 2 in the history.
    EXPECT_EQ(m.historySize(), std::size_t(2));
    EXPECT(m.cell(0, 2).codePoint == U'z');
    EXPECT_EQ(m.cursor().row, 2);
    m.resize(40, 3); // 0123456789ABCDE|xy|z: all fit, no history, rows at the top.
    EXPECT_EQ(m.historySize(), std::size_t(0));
    EXPECT(m.cell(0, 0).codePoint == U'0');
    EXPECT(m.cell(0, 1).codePoint == U'x');
    EXPECT(m.cell(0, 2).codePoint == U'z');
    EXPECT_EQ(m.cursor().row, 2);
    EXPECT_EQ(m.cursor().column, 1);
}

TEST_CASE("Live rows: blank rows below the cursor are not content") {
    TerminalModel m(10, 6, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"abcdefghijkl"); // Two rows, the cursor in the second.
    m.resize(5, 6);          // abcde|fghij|kl: three rows at the top, three blank rows below.
    EXPECT_EQ(m.historySize(), std::size_t(0));
    EXPECT_EQ(m.cursor().row, 2);
    EXPECT_EQ(m.cursor().column, 2);
    EXPECT(m.cell(0, 3).codePoint == U' ');
}

TEST_CASE("Live rows: the saved cursor follows its text") {
    TerminalModel m(10, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    m.write(U"abcdefghijklmno");
    m.moveCursor(2, 1);
    m.saveCursor(); // On the "m".
    m.moveCursor(0, 3);
    m.resize(6, 4);
    m.restoreCursor();
    EXPECT(m.cell(m.cursor().column, std::size_t(m.cursor().row)).codePoint == U'm');
}

TEST_CASE("Live rows: a scrolled-back reader keeps reading the same text") {
    TerminalModel m(20, 5, 400);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    for (int i = 0; i < 60; ++i) {
        m.write(U"line-" + std::u32string(1, char32_t(U'A' + i % 26)) + U"-" + std::u32string(10, char32_t(U'a' + i % 26)));
        m.carriageReturn();
        m.lineFeed();
    }
    m.scrollBy(3);
    m.advanceViewport(10);
    const auto before = m.frame(16);
    EXPECT(before.cells.rows() > 0);
    const char32_t key = before.cells.row(0).data()[5].codePoint;
    m.resize(11, 5);
    m.advanceViewport(10);
    const auto after = m.frame(16);
    std::u32string top;
    for (std::size_t x = 0; x < 11; ++x) top.push_back(after.cells.row(0).data()[x].codePoint);
    EXPECT(top.find(key) != std::u32string::npos);
    EXPECT(!m.viewport().atTail());
}

TEST_CASE("Live rows: a long content below the cursor keeps the cursor on the screen") {
    TerminalModel m(20, 4, 100);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    for (int i = 0; i < 4; ++i) {
        m.moveCursor(0, i);
        m.write(std::u32string(20, char32_t(U'a' + i)));
    }
    m.moveCursor(3, 0); // Cursor on the first row, text below.
    m.resize(3, 4);
    EXPECT(m.cursor().row >= 0 && m.cursor().row <= 3);
    EXPECT(m.cell(m.cursor().column, std::size_t(m.cursor().row)).codePoint == U'a');
}

TEST_CASE("Live rows: the model stays consistent under a random mix of output and sizes") {
    std::mt19937 rng(7);
    for (int round = 0; round < 40; ++round) {
        TerminalModel m(8 + rng() % 40, 3 + rng() % 8, 2000);
        m.setLiveReflow(round % 2 ? TerminalModel::LiveReflow::All : TerminalModel::LiveReflow::AboveCursor);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
        std::string typed; // Text of the lines written, to check what the history shows.
        for (int step = 0; step < 40; ++step) {
            switch (rng() % 4) {
            case 0: {
                std::u32string l;
                for (std::size_t k = 0, n = rng() % 70; k < n; ++k) l += char32_t(U'a' + rng() % 26);
                m.write(l);
                break;
            }
            case 1:
                m.carriageReturn();
                m.lineFeed();
                break;
            case 2:
                m.resize(1 + rng() % 70, m.rows());
                break;
            default:
                m.resize(m.columns(), 2 + rng() % 10);
                break;
            }
            EXPECT(m.cursor().row >= 0 && std::size_t(m.cursor().row) < m.rows());
            EXPECT(m.cursor().column >= 0 && std::size_t(m.cursor().column) < m.columns());
            if (rng() % 5 == 0) m.advanceViewport(0.2);
        }
        m.finishReflow();
        const auto f = m.frame(16);
        EXPECT(f.cells.rows() <= m.rows() + 1);
    }
}

TEST_CASE("Live rows: all the text survives any width, in order") {
    for (const auto mode : {TerminalModel::LiveReflow::AboveCursor, TerminalModel::LiveReflow::All}) {
    TerminalModel m(30, 6, 5000);
    m.setLiveReflow(mode);
    m.setLiveReflow(TerminalModel::LiveReflow::All);
    std::string expected;
    std::mt19937 rng(3);
    for (int i = 0; i < 60; ++i) {
        std::u32string l;
        for (std::size_t k = 0, n = 1 + rng() % 90; k < n; ++k) l += char32_t(U'a' + rng() % 26);
        l += char32_t(U'0' + i % 10);
        for (const char32_t c : l) expected += char(c);
        expected += "\n";
        m.write(l);
        m.carriageReturn();
        m.lineFeed();
    }
    expected.pop_back(); // The newline before the cursor's (empty) last line is trimmed by canon.
    for (const std::size_t w : {13u, 7u, 3u, 29u, 80u, 11u, 30u, 2u, 45u}) {
        m.resize(w, 6);
        m.finishReflow();
        EXPECT_EQ(canon(m, true), expected + "\n");
    }
    }
}

TEST_CASE("History capacity: the oldest physical rows go, never more") {
    TerminalModel m(20, 4, 30);
    std::vector<std::u32string> lines;
    for (int i = 0; i < 25; ++i) lines.push_back(std::u32string(35, char32_t(U'a' + i % 26)));
    fill(m, lines);
    EXPECT(m.historySize() <= 30);
    m.resize(7, 4); // 35 cells = 5 rows each: far more rows than fit.
    m.finishReflow();
    EXPECT(m.historySize() <= 30);
    EXPECT(m.historySize() >= 25);
    // The newest lines are intact (their text reads back whole from the first complete line).
    const auto text = canon(m);
    EXPECT(text.find(std::string(35, char('a' + 24 % 26))) != std::string::npos);
    // The oldest line may be cut at its start (evicted rows), never in the middle.
    const auto firstLine = text.substr(0, text.find('\n'));
    EXPECT(firstLine.size() % 7 == 0 || firstLine.size() <= 35);
}

TEST_CASE("Cursor, saved cursor and margins keep their meaning") {
    TerminalModel m(12, 5, 200);
    fill(m, sample(20, 40, 3));
    m.moveCursor(7, 2);
    m.saveCursor();
    m.setScrollRegion(1, 3);
    m.resize(5, 5);
    EXPECT(m.cursor().row <= 4); // Stays inside the screen.
    EXPECT(m.cursor().column <= 4);
    EXPECT_EQ(m.scrollTop(), 0);
    EXPECT_EQ(m.scrollBottom(), 4);
    m.restoreCursor();
    EXPECT(m.cursor().column <= 4 && m.cursor().row <= 4);
}

TEST_CASE("The reader keeps reading the same text") {
    TerminalModel m(30, 5, 400);
    std::vector<std::u32string> lines;
    for (int i = 0; i < 80; ++i) lines.push_back(U"line " + std::u32string(1, char32_t(U'A' + i % 26)) + std::u32string(20, char32_t(U'a' + i % 26)));
    fill(m, lines);
    m.scrollBy(30);
    m.advanceViewport(10);
    const auto before = m.frame(16);
    EXPECT(before.cells.rows() > 0);
    const char32_t marker = before.cells.row(0).data()[5].codePoint;
    const char32_t second = before.cells.row(0).data()[6].codePoint;
    m.resize(12, 5);
    m.finishReflow();
    m.advanceViewport(10);
    const auto after = m.frame(16);
    // The first visible row is a row of the same logical line as before (its text starts with "line ").
    std::u32string top;
    for (std::size_t x = 0; x < 12; ++x) top.push_back(after.cells.row(0).data()[x].codePoint);
    EXPECT(top.find(marker) != std::u32string::npos || top.find(second) != std::u32string::npos);
    EXPECT(!m.viewport().atTail());
    // A reader at the tail stays at the tail.
    m.followTail();
    m.advanceViewport(10);
    m.resize(30, 5);
    EXPECT(m.viewport().atTail());
}

TEST_CASE("A width change bumps the selection epoch, the slices do not") {
    TerminalModel m(20, 4, 6000);
    fill(m, sample(2500, 50, 5));
    const auto e0 = m.selectionEpoch();
    m.resize(9, 4);
    EXPECT(m.selectionEpoch() != e0);
    EXPECT(m.reflowPending());
    const auto e1 = m.selectionEpoch();
    const auto first = m.firstRetainedRow();
    const auto h = m.historySize();
    m.advanceViewport(0.2);
    EXPECT_EQ(m.selectionEpoch(), e1);
    EXPECT(m.historySize() > h);
    // Row ids of the rows that were there stay the same: the new rows come with smaller ids.
    EXPECT(m.firstRetainedRow() < first);
    EXPECT_EQ(m.firstRetainedRow() + m.historySize(), first + h);
}

// ---- Lazy reflow -----------------------------------------------------------------------------

TEST_CASE("Lazy: the newest rows at once, the rest after the size was quiet") {
    TerminalModel m(40, 6, 40000);
    const auto lines = sample(6000, 70, 21);
    fill(m, lines);
    const auto before = canon(m);
    m.resize(23, 6);
    EXPECT(m.reflowPending());
    EXPECT(m.historySize() < 3000); // Only the newest part is there yet.
    // Not quiet yet: nothing happens.
    const auto h = m.historySize();
    m.advanceViewport(0.05);
    EXPECT_EQ(m.historySize(), h);
    // Quiet: slices, then done.
    int frames = 0;
    while (m.reflowPending() && frames < 1000) {
        m.advanceViewport(0.2);
        ++frames;
    }
    EXPECT(!m.reflowPending());
    EXPECT(frames > 1 && frames < 100);
    EXPECT_EQ(canon(m), before);
}

TEST_CASE("Lazy: finishing equals laying everything out at once") {
    // Reference: the same output reflowed by a model small enough to do it all at once is not
    // possible, so compare against the algorithm on the original rows through a width round trip.
    TerminalModel a(40, 6, 40000), b(40, 6, 40000);
    const auto lines = sample(5000, 90, 22);
    fill(a, lines);
    fill(b, lines);
    a.resize(17, 6);
    a.finishReflow();
    b.resize(17, 6);
    int frames = 0;
    while (b.reflowPending() && frames++ < 1000) b.advanceViewport(0.2);
    EXPECT_EQ(a.historySize(), b.historySize());
    for (std::size_t y = 0; y < a.historySize() + a.rows(); y += 7) {
        const auto ida = a.firstRetainedRow() + y, idb = b.firstRetainedRow() + y;
        for (std::size_t x = 0; x < 17; ++x) EXPECT(a.textCodePoint({ida, x}) == b.textCodePoint({idb, x}));
        EXPECT_EQ(a.textRowWrapped(ida), b.textRowWrapped(idb));
    }
}

TEST_CASE("Lazy: another width change while rows wait") {
    TerminalModel m(40, 6, 40000);
    const auto lines = sample(5000, 80, 23);
    fill(m, lines);
    const auto before = canon(m);
    m.resize(19, 6);
    EXPECT(m.reflowPending());
    m.advanceViewport(0.2); // One slice.
    m.resize(31, 6);        // The size changes again before it finished.
    EXPECT(m.reflowPending());
    m.resize(11, 6);
    m.resize(40, 6);
    m.finishReflow();
    EXPECT(!m.reflowPending());
    EXPECT_EQ(canon(m), before);
}

TEST_CASE("Lazy: a drag changes the width every step and the history stays whole") {
    TerminalModel m(60, 6, 40000);
    const auto lines = sample(6000, 100, 24);
    fill(m, lines);
    const auto before = canon(m);
    for (std::size_t w = 59; w >= 20; --w) {
        m.resize(w, 6);
        m.advanceViewport(0.016); // Frames while dragging: never quiet.
    }
    for (std::size_t w = 21; w <= 60; ++w) {
        m.resize(w, 6);
        m.advanceViewport(0.016);
    }
    m.finishReflow();
    EXPECT_EQ(canon(m), before);
}

TEST_CASE("Lazy: output while rows wait, and a full ring drops them without harm") {
    TerminalModel m(40, 6, 5000);
    fill(m, sample(4500, 60, 25));
    m.resize(13, 6);
    EXPECT(m.reflowPending());
    // New output keeps coming and fills the ring.
    for (int i = 0; i < 12000; ++i) {
        m.write(U"more output that wraps around the narrow screen");
        m.carriageReturn();
        m.lineFeed();
        if (i % 500 == 0) m.advanceViewport(0.2);
    }
    EXPECT(m.historySize() <= 5000);
    m.finishReflow();
    EXPECT(!m.reflowPending());
    const auto f = m.frame(16); // Still consistent.
    EXPECT(f.cells.rows() > 0);
}

TEST_CASE("Lazy: clearing the scrollback drops what waits") {
    TerminalModel m(40, 6, 40000);
    fill(m, sample(5000, 60, 26));
    m.resize(15, 6);
    EXPECT(m.reflowPending());
    m.eraseScrollback();
    EXPECT(!m.reflowPending());
    EXPECT_EQ(m.historySize(), std::size_t(0));
    m.resize(30, 6);
    EXPECT(!m.reflowPending());
}

TEST_CASE("Lazy: height changes with rows waiting") {
    TerminalModel m(40, 8, 40000);
    const auto lines = sample(5000, 60, 27);
    fill(m, lines);
    m.resize(17, 8);
    EXPECT(m.reflowPending());
    m.resize(17, 12);
    m.resize(17, 5);
    m.resize(40, 8);
    m.finishReflow();
    // The live rows were clipped on the way, but the saved text is the same text.
    const auto text = canon(m);
    const auto original = join(lines);
    EXPECT(original.find(text.substr(text.size() / 2, 200)) != std::string::npos);
    EXPECT(!m.reflowPending());
}

TEST_CASE("Property: random writes and widths never break the text of the history") {
    std::mt19937 rng(99);
    for (int round = 0; round < 30; ++round) {
        TerminalModel m(10 + rng() % 40, 4 + rng() % 6, 20000);
        const auto lines = sample(100 + rng() % 500, 5 + rng() % 120, unsigned(rng()));
        fill(m, lines);
        const auto before = canon(m);
        for (int step = 0; step < 12; ++step) {
            m.resize(4 + rng() % 87, m.rows());
            if (rng() % 3 == 0) m.advanceViewport(0.2);
            if (rng() % 7 == 0) {
                m.scrollBy(double(rng() % 40));
                m.advanceViewport(0.1);
            }
            const auto f = m.frame(16);
            EXPECT(f.cells.rows() <= m.rows() + 1);
        }
        m.finishReflow();
        EXPECT_EQ(canon(m), before);
    }
}

TEST_CASE("AboveCursor (default): the rows above the cursor's line are laid out again, its own rows are clipped") {
    TerminalModel m(10, 6, 100);
    EXPECT(m.liveReflow() == TerminalModel::LiveReflow::AboveCursor);
    m.write(U"0123456789ABCDE"); // Wrapped line above.
    m.carriageReturn();
    m.lineFeed();
    m.write(U"pr$ ty"); // The cursor's line: one row.
    m.resize(5, 6);
    // Above: 01234|56789|ABCDE (3 rows). The prompt row is clipped to "promp", not joined or cut in two.
    const auto first = m.firstRetainedRow();
    const auto rows = m.historySize() + m.rows();
    std::size_t promptRow = rows;
    for (std::size_t r = 0; r < rows; ++r) {
        if (m.textCodePoint({first + r, 0}) == std::optional<char32_t>(U'p')) promptRow = r;
    }
    EXPECT_EQ(promptRow, std::size_t(3));
    EXPECT(!m.textRowWrapped(first + 3));
    EXPECT_EQ(std::size_t(m.cursor().row), promptRow - m.historySize());
    EXPECT_EQ(m.cursor().column, 4);
    EXPECT(m.textRowWrapped(first + 1)); // The lines above follow the new width.
}

TEST_CASE("AboveCursor: a shell that redraws its prompt after the resize leaves no copy behind") {
    // What bash does on SIGWINCH: carriage return, erase to the end of the line, write prompt and input again.
    const auto count = [](TerminalModel &m, char32_t c) {
        std::size_t n = 0;
        const auto first = m.firstRetainedRow();
        for (std::size_t r = 0; r < m.historySize() + m.rows(); ++r)
            for (std::size_t x = 0; x < m.columns(); ++x)
                if (m.textCodePoint({first + r, x}) == std::optional<char32_t>(c)) ++n;
        return n;
    };
    const std::u32string line = std::u32string(29, U'P') + U"$ echo hello";
    for (const auto mode : {TerminalModel::LiveReflow::AboveCursor, TerminalModel::LiveReflow::All}) {
        TerminalModel m(80, 8, 100);
        m.setLiveReflow(mode);
        m.write(line);
        m.resize(20, 8);
        m.carriageReturn();
        m.eraseLine();
        m.write(line);
        if (mode == TerminalModel::LiveReflow::AboveCursor) {
            EXPECT_EQ(count(m, U'P'), std::size_t(29));
        }
        else {
            EXPECT(count(m, U'P') > 29); // VTE-like behaviour shows the copy: the reason for the default.
        }
        // The same when widening again: the shell knows its prompt took three rows, goes up two and
        // clears from there (it does not know about reflow).
        m.resize(80, 8);
        m.moveCursor(0, m.cursor().row - 2);
        m.eraseDisplay();
        m.write(line);
        if (mode == TerminalModel::LiveReflow::AboveCursor) {
            EXPECT_EQ(count(m, U'P'), std::size_t(29));
        }
    }
}

TEST_CASE("AboveCursor: a wrapped cursor line keeps its rows when the width grows, its flags stay") {
    TerminalModel m(6, 5, 100);
    m.write(U"abcdefghi"); // abcdef|ghi, cursor behind the i.
    m.resize(12, 5);
    EXPECT(m.textRowWrapped(m.firstRetainedRow()));
    EXPECT(m.cell(0, 1).codePoint == U'g');
    EXPECT_EQ(m.cursor().row, 1);
    EXPECT_EQ(m.cursor().column, 3);
}

TEST_SUIT_END
