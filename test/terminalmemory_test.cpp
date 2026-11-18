#include "mls-unit-test/unittest.h"
#include "terminalmemory.h"

#include <algorithm>
#include <deque>
#include <limits>

TEST_SUIT_BEGIN(TerminalMemory)

TEST_CASE("History starts empty and clips or pads appended rows") {
    TerminalMemory memory(3, 5, 2);
    EXPECT_EQ(memory.size(), std::size_t(0));
    EXPECT_EQ(memory.capacity(), std::size_t(5));
    EXPECT_THROW(memory.row(0), std::out_of_range);
    memory.append(U"abcde", {1, 2, 3}, {4, 5, 6});
    memory.append(U"x");
    EXPECT(memory.row(0).data()[2].codePoint == U'c');
    EXPECT_EQ(memory.row(0).data()[1].foreground.g, 2);
    EXPECT_EQ(memory.row(0).data()[1].background.b, 6);
    EXPECT(memory.row(1).data()[1].codePoint == U' ');
    EXPECT_THROW(memory.row(2), std::out_of_range);
}

TEST_CASE(
    "Exact capacity evicts one oldest row without shifting surviving cells") {
    TerminalMemory memory(1, 5, 2); // Partial last block.
    for (char32_t ch = U'a'; ch <= U'e'; ++ch) {
        memory.append(std::u32string_view(&ch, 1));
    }
    const auto *survivor = memory.row(1).data();
    memory.append(U"f");
    EXPECT_EQ(memory.size(), std::size_t(5));
    EXPECT_EQ(memory.row(0).data(), survivor);
    EXPECT(memory.row(0).data()[0].codePoint == U'b');
    EXPECT(memory.row(4).data()[0].codePoint == U'f');
}

TEST_CASE(
    "Visible row descriptors cross block boundaries and physical wraparound") {
    TerminalMemory memory(1, 5, 2);
    for (char32_t ch = U'a'; ch <= U'h'; ++ch) {
        memory.append(std::u32string_view(&ch, 1));
    }
    const auto view = memory.view(0, 100);
    EXPECT_EQ(view.rows(), std::size_t(5));
    for (std::size_t y = 0; y < view.rows(); ++y) {
        EXPECT(view.row(y).data()[0].codePoint == U'd' + y);
        EXPECT_EQ(view.row(y).data(), memory.row(y).data());
    }
    const auto tail = memory.view(3, 100);
    EXPECT_EQ(tail.rows(), std::size_t(2));
    EXPECT(tail.row(0).data()[0].codePoint == U'g');
    EXPECT_EQ(memory.view(5, 10).rows(), std::size_t(0));
    EXPECT_THROW(memory.view(6, 1), std::out_of_range);
}

TEST_CASE(
    "Formatted input can alias the row being evicted without stale padding") {
    TerminalMemory memory(3, 1, 64);
    memory.append(U"abc", {10, 20, 30}, {40, 50, 60});
    memory.append(FStringView(memory.row(0).data() + 1, 2), FChar{U'.'});
    EXPECT(memory.row(0).data()[0].codePoint == U'b');
    EXPECT(memory.row(0).data()[1].codePoint == U'c');
    EXPECT(memory.row(0).data()[2].codePoint == U'.');
    EXPECT_EQ(memory.row(0).data()[0].background.b, 60);
    memory.append(U"");
    EXPECT(memory.row(0).data()[0].codePoint == U' ');
}

TEST_CASE("Resize preserves logical order after wraparound without reflow") {
    TerminalMemory memory(2, 3, 2);
    memory.append(U"aa");
    memory.append(U"bb");
    memory.append(U"cc");
    memory.append(U"dd");
    memory.resizeColumns(4, FChar{U'.'});
    EXPECT(memory.row(0).data()[0].codePoint == U'b');
    EXPECT(memory.row(2).data()[1].codePoint == U'd');
    EXPECT(memory.row(1).data()[3].codePoint == U'.');
    memory.append(U"eeee");
    memory.resizeColumns(1);
    EXPECT(memory.row(0).data()[0].codePoint == U'c');
    EXPECT(memory.row(2).data()[0].codePoint == U'e');
}

TEST_CASE("Clear reuses storage and zero-width histories are valid") {
    TerminalMemory memory(0, 2, 1);
    memory.append(U"ignored");
    memory.append(U"ignored");
    memory.append(U"ignored");
    EXPECT_EQ(memory.size(), std::size_t(2));
    EXPECT_EQ(memory.row(0).size(), std::size_t(0));
    EXPECT_EQ(memory.view(0, 2).columns(), std::size_t(0));
    memory.clear();
    EXPECT_EQ(memory.size(), std::size_t(0));
    memory.resizeColumns(2);
    memory.append(U"ok");
    EXPECT(memory.row(0).data()[1].codePoint == U'k');
    memory.clear();
    memory.append(U"x");
    EXPECT(memory.row(0).data()[1].codePoint == U' ');
}

TEST_CASE("Invalid sizes fail and failed resize preserves history and views") {
    EXPECT_THROW((TerminalMemory(2, 0)), std::invalid_argument);
    EXPECT_THROW((TerminalMemory(2, 2, 0)), std::invalid_argument);
    TerminalMemory memory(2, 3);
    memory.append(U"ok");
    const auto view = memory.view(0, 1);
    EXPECT_THROW(memory.resizeColumns(std::numeric_limits<std::size_t>::max()),
                 std::length_error);
    EXPECT_EQ(memory.columns(), std::size_t(2));
    EXPECT(view.row(0).data()[1].codePoint == U'k');
}

TEST_CASE(
    "Repeated wraps match a reference queue across block sizes and widths") {
    for (const std::size_t blockRows : {1, 2, 4, 64}) {
        for (const std::size_t capacity : {1, 3, 8}) {
            TerminalMemory memory(3, capacity, blockRows);
            std::deque<FString> expected;
            std::size_t width = 3;
            for (int i = 0; i < 200; ++i) {
                if (i % 17 == 0) {
                    width = std::size_t(i % 7);
                    memory.resizeColumns(width);
                    for (auto &row : expected) {
                        row.resize(width);
                    }
                }
                const std::u32string text(std::size_t(i % 9),
                                          char32_t(U'A' + i % 26));
                memory.append(text);
                FString row(width);
                for (std::size_t x = 0; x < std::min(width, text.size()); ++x) {
                    row[x].codePoint = text[x];
                }
                if (expected.size() == capacity) {
                    expected.pop_front();
                }
                expected.push_back(row);
                EXPECT_EQ(memory.size(), expected.size());
                const auto view = memory.view(0, memory.size());
                for (std::size_t y = 0; y < expected.size(); ++y) {
                    for (std::size_t x = 0; x < width; ++x) {
                        EXPECT(view.row(y).data()[x].codePoint ==
                               expected[y][x].codePoint);
                    }
                }
            }
        }
    }
}

TEST_SUIT_END
