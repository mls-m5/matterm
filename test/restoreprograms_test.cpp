#include "cellbuffer.h"
#include "mls-unit-test/unittest.h"
#include "restoreprograms.h"
#include "ui/programlist.h"

#include <string>
#include <vector>

using namespace frontend;

namespace {
ProgramCandidate candidate(PaneId pane,
                           const std::string &cwd,
                           std::vector<std::string> command) {
    ProgramCandidate c;
    c.pane = pane;
    c.saved.cwd = cwd;
    c.saved.command = std::move(command);
    return c;
}

std::string rowText(const CellBuffer &cells, std::size_t y) {
    std::string out;
    for (std::size_t x = 0; x < cells.columns(); ++x) {
        const char32_t cp = cells.at(x, y).codePoint;
        out += cp < 128 ? char(cp) : '?';
    }
    return out;
}

std::string allText(const CellBuffer &cells) {
    std::string out;
    for (std::size_t y = 0; y < cells.rows(); ++y)
        out += rowText(cells, y) + "\n";
    return out;
}

KeyChord key(int k) {
    KeyChord c;
    c.key = k;
    return c;
}
} // namespace

TEST_SUIT_BEGIN(RestorePrograms)

TEST_CASE("The question counts the programs, in the singular too") {
    EXPECT_EQ(startQuestion(3), std::string("Start 3 programs again?"));
    EXPECT_EQ(startQuestion(1), std::string("Start 1 program again?"));
}

TEST_CASE("Only panes with a plain command, a folder that is there and a live "
          "terminal are offered") {
    std::vector<ProgramCandidate> all = {
        candidate(1, "/home/a/src", {"vim", "notes.txt"}),
        candidate(2, "/home/a", {}), // Nothing was running.
        candidate(3, "/home/a", {"make", "-j8"}),
        candidate(4, "/gone", {"top"}),          // Folder gone.
        candidate(5, "/home/a", {"vi\x1b[2Jm"}), // A command that is refused.
        candidate(6, "/home/a", {"htop"}),       // Terminal not created.
    };
    all[3].folderGone = true;
    all[5].noTerminal = true;
    const auto offered = offeredPrograms(all);
    EXPECT_EQ(offered.size(), std::size_t(2));
    EXPECT_EQ(offered[0].pane, PaneId(1));
    EXPECT_EQ(offered[1].pane, PaneId(3));
    EXPECT_EQ(offered[0].command, std::string("vim notes.txt"));
}

TEST_CASE("The line says the short folder and the quoted command; what is "
          "typed ends with Enter") {
    const auto offered = offeredPrograms(
        {candidate(1, "/home/a/src/proj", {"echo", "two words"})});
    EXPECT_EQ(offered.size(), std::size_t(1));
    EXPECT(offered[0].line.find("src/proj") != std::string::npos);
    EXPECT(offered[0].line.find("'two words'") != std::string::npos);
    EXPECT(offered[0].line.find("/home/a") == std::string::npos); // Short.
    EXPECT_EQ(startInput(offered[0]), std::string("echo 'two words'\r"));
}

TEST_CASE("No programs, no question") {
    EXPECT(offeredPrograms({}).empty());
    EXPECT(offeredPrograms({candidate(1, "/", {})}).empty());
}

TEST_CASE("The answer is Not now unless the owner chooses otherwise") {
    ui::ProgramListDialog dialog("Start 1 program again?",
                                 {"a  vim"},
                                 restoreaction::start,
                                 restoreaction::skip);
    EXPECT_EQ(dialog.selected(), std::size_t(1));
    auto out = dialog.key(key(keys::Enter));
    EXPECT(out.close && out.action == restoreaction::skip);
    ui::ProgramListDialog second(
        "q", {"x"}, restoreaction::start, restoreaction::skip);
    out = second.key(key(keys::Escape));
    EXPECT(out.close && out.action == restoreaction::skip);
    ui::ProgramListDialog third(
        "q", {"x"}, restoreaction::start, restoreaction::skip);
    third.key(key(keys::Left));
    EXPECT_EQ(third.selected(), std::size_t(0));
    out = third.key(key(keys::Enter));
    EXPECT(out.close && out.action == restoreaction::start);
    EXPECT(!third.key(key('x')).close); // Another key does nothing.
}

TEST_CASE("Tab changes the button and the buttons can be clicked") {
    ui::ProgramListDialog dialog(
        "q", {"x"}, restoreaction::start, restoreaction::skip);
    dialog.resize(40, 8);
    dialog.key(key(keys::Tab));
    EXPECT_EQ(dialog.selected(), std::size_t(0));
    CellBuffer cells(40, 8);
    CursorState cursor;
    dialog.render(cells, cursor);
    const std::string text = allText(cells);
    EXPECT(text.find("Start them") != std::string::npos);
    EXPECT(text.find("Not now") != std::string::npos);
    EXPECT(dialog.click(0, 0).action.empty()); // Not on the buttons.
}

TEST_CASE("The question and the programs are drawn, a long list scrolls") {
    std::vector<std::string> lines;
    for (int i = 0; i < 20; ++i)
        lines.push_back("dir  prog" + std::to_string(i));
    ui::ProgramListDialog dialog("Start 20 programs again?",
                                 lines,
                                 restoreaction::start,
                                 restoreaction::skip);
    const auto size = dialog.wanted(80, 30);
    EXPECT(size.rows <= 14); // The list is not the whole window.
    dialog.resize(size.columns, size.rows);
    CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
    CursorState cursor;
    dialog.render(cells, cursor);
    std::string text = allText(cells);
    EXPECT(text.find("Start 20 programs again?") != std::string::npos);
    EXPECT(text.find("prog0") != std::string::npos);
    EXPECT(text.find("prog19") == std::string::npos);
    for (int i = 0; i < 30; ++i)
        dialog.key(key(keys::Down));
    dialog.render(cells, cursor);
    text = allText(cells);
    EXPECT(text.find("prog19") != std::string::npos);
    EXPECT(text.find("prog0 ") == std::string::npos);
    dialog.wheel(5); // Up.
    EXPECT(dialog.firstLine() < 14);
    dialog.key(key(keys::PageUp));
    dialog.render(cells, cursor);
    // A tiny window must not crash.
    ui::ProgramListDialog tiny(
        "q", lines, restoreaction::start, restoreaction::skip);
    tiny.resize(5, 3);
    CellBuffer small(5, 3);
    tiny.render(small, cursor);
}

TEST_SUIT_END
