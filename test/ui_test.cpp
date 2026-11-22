#include "mls-unit-test/unittest.h"
#include "ui/commandpalette.h"
#include "ui/dialogs.h"
#include "ui/shortcuts.h"

#include <string>
#include <vector>

using namespace frontend;
using namespace frontend::ui;

namespace {
std::string rowText(const CellBuffer &cells, int row) {
    std::string s;
    for (std::size_t x = 0; x < cells.columns(); ++x) {
        const auto cp = cells.at(x, std::size_t(row)).codePoint;
        s += cp < 128 ? char(cp) : '?';
    }
    return s;
}

struct Palette80 {
    CommandPalette palette{buildCommandRegistry()};
    int columns = 80, rows = 24;
    CellSize size;
    Palette80() {
        size = palette.wanted(columns, rows);
        palette.resize(size.columns, size.rows);
    }
    void type(const std::string &text) {
        for (const char c : text) palette.key({c, false, false, false});
    }
};
} // namespace

TEST_SUIT_BEGIN(Ui)

TEST_CASE("TextInput edits at the cursor and refuses control characters") {
    TextInput t(5);
    EXPECT(t.insert(U'a'));
    EXPECT(t.insert(U'c'));
    t.left();
    EXPECT(t.insert(U'b'));
    EXPECT_EQ(t.utf8(), std::string("abc"));
    EXPECT_EQ(t.cursor(), std::size_t(2));
    EXPECT(!t.insert(U'\n'));
    EXPECT(!t.insert(U'\x7f'));
    EXPECT(!t.insert(0xd800));
    EXPECT(t.insert(U'å'));
    EXPECT_EQ(t.utf8(), std::string("ab\xc3\xa5" "c"));
    EXPECT(t.insert(U'x'));
    EXPECT(!t.insert(U'y')); // Length limit 5.
    t.home();
    EXPECT(!t.backspace());
    EXPECT(t.erase());
    EXPECT_EQ(t.utf8(), std::string("b\xc3\xa5" "xc"));
    t.end();
    EXPECT(t.backspace());
    EXPECT(!t.erase());
    EXPECT(t.clear());
    EXPECT(t.empty() && t.cursor() == 0);
}

TEST_CASE("TextInput scrolls so that the cursor stays visible") {
    TextInput t;
    for (int i = 0; i < 20; ++i) t.insert(U'a');
    EXPECT_EQ(t.firstVisible(30), std::size_t(0));
    EXPECT_EQ(t.firstVisible(10), std::size_t(11)); // cursor 20, width 10
    t.home();
    EXPECT_EQ(t.firstVisible(10), std::size_t(0));
    EXPECT_EQ(t.firstVisible(0), std::size_t(0));
}

TEST_CASE("ListView keeps the selection in view, wraps and pages") {
    ListView l;
    l.setCount(20);
    l.setVisible(5);
    EXPECT_EQ(l.selected(), std::size_t(0));
    l.up(); // Wraps to the end.
    EXPECT_EQ(l.selected(), std::size_t(19));
    EXPECT_EQ(l.first(), std::size_t(15));
    l.down();
    EXPECT(l.selected() == 0 && l.first() == 0);
    l.pageDown();
    EXPECT_EQ(l.selected(), std::size_t(5));
    EXPECT_EQ(l.first(), std::size_t(1));
    l.pageUp();
    l.pageUp();
    EXPECT_EQ(l.selected(), std::size_t(0));
    l.scroll(100);
    EXPECT_EQ(l.first(), std::size_t(15));
    EXPECT(l.selected() >= 15 && l.selected() < 20);
    l.scroll(-100);
    EXPECT_EQ(l.first(), std::size_t(0));
    l.setCount(0);
    l.down();
    l.up();
    l.pageDown();
    EXPECT_EQ(l.selected(), std::size_t(0));
    l.setCount(3);
    l.setVisible(10);
    l.select(99);
    EXPECT_EQ(l.selected(), std::size_t(2));
    EXPECT_EQ(l.first(), std::size_t(0));
}

TEST_CASE("The palette lists everything first, then filters as you type") {
    Palette80 p;
    const auto all = p.palette.resultCount();
    EXPECT_GT(all, std::size_t(10));
    p.type("new tab");
    EXPECT(p.palette.resultCount() < all);
    EXPECT(p.palette.selectedEntry() != nullptr);
    EXPECT_EQ(p.palette.selectedEntry()->name, std::string("New tab"));
    p.palette.key({keys::Backspace, false, false, false});
    EXPECT_EQ(p.palette.query(), std::string("new ta"));
    p.type("zzzzzz");
    EXPECT_EQ(p.palette.resultCount(), std::size_t(0));
    EXPECT(p.palette.selectedEntry() == nullptr);
    EXPECT(!p.palette.key({keys::Enter, false, false, false}).close); // Nothing to run.
}

TEST_CASE("Enter runs the selected command, Escape closes, Up and Down move") {
    Palette80 p;
    p.type("split side");
    auto out = p.palette.key({keys::Enter, false, false, false});
    EXPECT(out.close);
    EXPECT(out.call.has_value());
    EXPECT(out.call->command == Command::SplitSideBySide);
    Palette80 q;
    const auto first = q.palette.selectedEntry();
    q.palette.key({keys::Down, false, false, false});
    EXPECT(q.palette.selectedEntry() != first);
    q.palette.key({keys::Up, false, false, false});
    EXPECT(q.palette.selectedEntry() == first);
    const auto esc = q.palette.key({keys::Escape, false, false, false});
    EXPECT(esc.close && !esc.call && esc.action.empty());
    // Entries without a call give their id as an action.
    std::vector<CommandEntry> extra;
    extra.push_back({"app.rename", "Rename tab", "Give the tab a name", std::nullopt, {}, {}});
    CommandPalette custom(buildCommandRegistry(PrefixKeys::Config{}, extra));
    custom.resize(60, 12);
    for (const char c : std::string("rename")) custom.key({c, false, false, false});
    const auto run = custom.key({keys::Enter, false, false, false});
    EXPECT(run.close && !run.call);
    EXPECT_EQ(run.action, std::string("app.rename"));
}

TEST_CASE("Ctrl keys: N and P move, U clears, others do nothing") {
    Palette80 p;
    p.type("tab");
    const auto before = p.palette.resultCount();
    const auto top = p.palette.selectedEntry();
    p.palette.key({'n', true, false, false});
    EXPECT(p.palette.selectedEntry() != top);
    p.palette.key({'p', true, false, false});
    EXPECT(p.palette.selectedEntry() == top);
    EXPECT(!p.palette.key({'x', true, false, false}).close);
    EXPECT_EQ(p.palette.resultCount(), before);
    p.palette.key({'u', true, false, false});
    EXPECT_EQ(p.palette.query(), std::string());
    EXPECT_GT(p.palette.resultCount(), before);
}

TEST_CASE("A click on an entry runs it; clicks elsewhere do nothing") {
    Palette80 p;
    p.type("new tab");
    EXPECT(!p.palette.click(0, 2).close);  // The border.
    EXPECT(!p.palette.click(5, 1).close);  // The input line.
    EXPECT(!p.palette.click(5, p.size.rows - 1).close);
    const auto out = p.palette.click(5, 2); // The first entry.
    EXPECT(out.close);
    EXPECT(out.call.has_value());
    EXPECT(out.call->command == Command::NewTab);
    // Below the last result: nothing.
    Palette80 q;
    q.type("new tab");
    EXPECT(!q.palette.click(5, q.size.rows - 2).close);
}

TEST_CASE("The palette draws its query, the matches and the keys, and a cursor") {
    Palette80 p;
    p.type("new");
    CellBuffer cells(std::size_t(p.size.columns), std::size_t(p.size.rows));
    CursorState cursor;
    p.palette.render(cells, cursor);
    EXPECT(rowText(cells, 1).find("> new") != std::string::npos);
    EXPECT(cursor.visible);
    EXPECT_EQ(cursor.row, 1);
    EXPECT_EQ(cursor.column, 3 + 3);
    const auto first = rowText(cells, 2);
    EXPECT(first.find("New tab") != std::string::npos);
    EXPECT(first.find("Ctrl+B c") != std::string::npos); // Its key, right aligned.
    // The selected row stands out from the next one.
    EXPECT(cells.at(2, 2).background.b != cells.at(2, 3).background.b ||
           cells.at(2, 2).background.g != cells.at(2, 3).background.g);
    // The border is drawn in cells of its own colour.
    EXPECT(cells.at(0, 0).background.b != cells.at(2, 2).background.b);
    EXPECT(cells.at(std::size_t(p.size.columns - 1), std::size_t(p.size.rows - 1)).background.b ==
           cells.at(0, 0).background.b);
}

TEST_CASE("No match is said; tiny windows never break the palette") {
    Palette80 p;
    p.type("qqqq");
    CellBuffer cells(std::size_t(p.size.columns), std::size_t(p.size.rows));
    CursorState cursor;
    p.palette.render(cells, cursor);
    EXPECT(rowText(cells, 2).find("No matching command") != std::string::npos);
    for (int c = 1; c <= 20; ++c) {
        for (int r = 1; r <= 8; ++r) {
            CommandPalette small(buildCommandRegistry());
            const auto s = small.wanted(c, r);
            EXPECT(s.columns >= 1 && s.columns <= c && s.rows >= 1 && s.rows <= r);
            small.resize(s.columns, s.rows);
            CellBuffer grid(std::size_t(s.columns), std::size_t(s.rows));
            CursorState cs;
            small.render(grid, cs);
            small.key({'a', false, false, false});
            small.click(s.columns / 2, s.rows / 2);
            small.wheel(1);
            small.wheel(-1);
            small.render(grid, cs);
        }
    }
}

TEST_CASE("Desktop keys are shown when preferred, the other kind as a fallback") {
    CommandPalette desktop(buildCommandRegistry(), true);
    desktop.resize(60, 12);
    for (const char c : std::string("new tab")) desktop.key({c, false, false, false});
    CellBuffer cells(60, 12);
    CursorState cursor;
    desktop.render(cells, cursor);
    EXPECT(rowText(cells, 2).find("Ctrl+Shift+T") != std::string::npos);
}

TEST_CASE("The wheel scrolls the list") {
    Palette80 p;
    EXPECT_EQ(p.palette.firstVisible(), std::size_t(0));
    p.palette.wheel(-1); // Towards the end.
    EXPECT_GT(p.palette.firstVisible(), std::size_t(0));
    p.palette.wheel(1);
    EXPECT_EQ(p.palette.firstVisible(), std::size_t(0));
}


    TEST_CASE("ConfirmDialog: the safe button is the default, Escape cancels, a click on a button answers") {
        ConfirmDialog d("Close this tab? vim notes.txt is still running.",
                        {{"Close", "close.confirm"}, {"Cancel", "close.cancel"}}, 1, 1);
        const auto size = d.wanted(80, 24);
        d.resize(size.columns, size.rows);
        CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
        CursorState cursor;
        d.render(cells, cursor);
        EXPECT(!cursor.visible);
        std::string all;
        for (int y = 0; y < size.rows; ++y) all += rowText(cells, y) + "\n";
        EXPECT(all.find("vim notes.txt") != std::string::npos);
        EXPECT(all.find("[ Close ]") != std::string::npos && all.find("[ Cancel ]") != std::string::npos);
        // Enter takes the initial button (Cancel).
        auto out = d.key({keys::Enter, false, false, false});
        EXPECT(out.close && out.action == "close.cancel");
        // Move left, then Enter: Close.
        d.key({keys::Left, false, false, false});
        EXPECT_EQ(d.selected(), std::size_t(0));
        out = d.key({keys::Enter, false, false, false});
        EXPECT(out.close && out.action == "close.confirm");
        // Escape is always the cancel button, whatever is selected.
        out = d.key({keys::Escape, false, false, false});
        EXPECT(out.close && out.action == "close.cancel");
        // Plain letters do nothing (no accidental Close).
        out = d.key({'c', false, false, false});
        EXPECT(!out.close && out.action.empty());
        // Clicking the "Close" label.
        int closeColumn = -1;
        const auto last = rowText(cells, size.rows - 2);
        closeColumn = int(last.find("Close"));
        EXPECT(closeColumn > 0);
        out = d.click(closeColumn, size.rows - 2);
        EXPECT(out.close && out.action == "close.confirm");
        EXPECT(!d.click(1, 1).close);
    }

    TEST_CASE("ConfirmDialog: long and hostile text is wrapped and shown printable") {
        std::string q = "Close this tab? ";
        q += std::string(200, 'x');
        q += " \x1b[2J still running.";
        ConfirmDialog d(q, {{"Close", "a"}, {"Cancel", "b"}}, 1, 1);
        const auto size = d.wanted(40, 12);
        EXPECT(size.columns <= 40 && size.rows <= 12);
        d.resize(size.columns, size.rows);
        CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
        CursorState cursor;
        d.render(cells, cursor);
        for (int y = 0; y < size.rows; ++y) {
            for (std::size_t x = 0; x < cells.columns(); ++x) EXPECT(cells.at(x, std::size_t(y)).codePoint >= U' ');
        }
        // A tiny window draws nothing and does not crash.
        d.resize(3, 2);
        CellBuffer tiny(3, 2);
        d.render(tiny, cursor);
    }

    TEST_CASE("TextPrompt answers prefix plus text; empty only when allowed; Escape closes") {
        TextPrompt p("Save session as", "work", "session.save:", false);
        const auto size = p.wanted(80, 24);
        p.resize(size.columns, size.rows);
        for (const char c : std::string("-2")) p.key({c, false, false, false});
        p.key({keys::Backspace, false, false, false});
        for (const char c : std::string("b")) p.key({c, false, false, false});
        auto out = p.key({keys::Enter, false, false, false});
        EXPECT(out.close);
        EXPECT_EQ(out.action, std::string("session.save:work-b"));
        TextPrompt e("Name", "", "tab.rename:", false);
        e.resize(40, 4);
        EXPECT(!e.key({keys::Enter, false, false, false}).close);
        TextPrompt a("Name", "x", "tab.rename:", true);
        a.resize(40, 4);
        a.key({keys::Backspace, false, false, false});
        out = a.key({keys::Enter, false, false, false});
        EXPECT(out.close && out.action == "tab.rename:");
        EXPECT(a.key({keys::Escape, false, false, false}).close);
        // Control characters never get in.
        TextPrompt c("n", "", "p:", true);
        c.resize(40, 4);
        c.key({1, false, false, false});
        c.key({'\n', false, false, false});
        EXPECT_EQ(c.current(), std::string());
        CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
        CursorState cursor;
        p.render(cells, cursor);
        EXPECT(cursor.visible && cursor.row == 2);
    }

    TEST_CASE("PickList: choose by key or click, scroll, empty list says so") {
        std::vector<PickList::Item> items;
        for (int i = 0; i < 25; ++i) items.push_back({"session" + std::to_string(i), std::to_string(i) + " tabs", "session.open:s" + std::to_string(i)});
        PickList l("Open session", items);
        const auto size = l.wanted(80, 24);
        l.resize(size.columns, size.rows);
        auto out = l.key({keys::Down, false, false, false});
        EXPECT(!out.close);
        out = l.key({keys::Enter, false, false, false});
        EXPECT(out.close && out.action == "session.open:s1");
        l.key({keys::PageDown, false, false, false});
        EXPECT(l.selected() > 1);
        CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
        CursorState cursor;
        l.render(cells, cursor);
        EXPECT(rowText(cells, 1).find("Open session") != std::string::npos);
        out = l.click(3, 2);
        EXPECT(out.close && out.action.rfind("session.open:", 0) == 0);
        EXPECT(l.key({keys::Escape, false, false, false}).close);
        PickList none("Open session", {}, "No saved sessions");
        none.resize(30, 4);
        CellBuffer c2(30, 4);
        none.render(c2, cursor);
        EXPECT(rowText(c2, 2).find("No saved sessions") != std::string::npos);
        EXPECT(!none.key({keys::Enter, false, false, false}).close);
    }

TEST_CASE("The palette shows each command's keys, also in a narrow window, and cuts the name instead") {
    for (const bool desktop : {true, false}) {
        CommandPalette p(buildCommandRegistry(), desktop);
        const auto size = p.wanted(40, 20);
        p.resize(size.columns, size.rows);
        CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
        CursorState cursor;
        p.render(cells, cursor);
        int withKeys = 0;
        for (int row = 2; row < size.rows - 1; ++row) {
            const auto text = rowText(cells, row);
            if (text.find("Ctrl+") != std::string::npos) ++withKeys;
        }
        EXPECT(withKeys >= 5);
    }
    // Wide enough for everything: "Keyboard shortcuts" and its keys on one line.
    CommandPalette p(buildCommandRegistry(), true);
    p.resize(64, 12);
    for (const char c : std::string("keyboard sh")) p.key({c, false, false, false});
    CellBuffer cells(64, 12);
    CursorState cursor;
    p.render(cells, cursor);
    EXPECT(rowText(cells, 2).find("Keyboard shortcuts") != std::string::npos);
    EXPECT(rowText(cells, 2).find("Ctrl+Shift+H") != std::string::npos);
}

TEST_CASE("The shortcut view lists commands with desktop and prefix keys, scrolls and closes") {
    ShortcutsView v(buildCommandRegistry(), true, true);
    const auto size = v.wanted(80, 24);
    v.resize(size.columns, size.rows);
    CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
    CursorState cursor;
    v.render(cells, cursor);
    EXPECT(rowText(cells, 1).find("Keyboard shortcuts") != std::string::npos);
    std::string all;
    for (int row = 2; row < size.rows - 1; ++row) all += rowText(cells, row) + "\n";
    EXPECT(all.find("New tab") != std::string::npos);
    EXPECT(all.find("Ctrl+Shift+T") != std::string::npos);
    EXPECT(all.find("Ctrl+B c") != std::string::npos);
    EXPECT(v.rowCount() > std::size_t(size.rows - 3)); // more than fits: it scrolls
    v.key({keys::PageDown, false, false, false});
    EXPECT(v.firstVisible() > 0);
    // The last page holds copy mode.
    v.key({keys::End, false, false, false});
    v.render(cells, cursor);
    all.clear();
    for (int row = 2; row < size.rows - 1; ++row) all += rowText(cells, row) + "\n";
    EXPECT(all.find("Leave") != std::string::npos);
    v.wheel(1);
    EXPECT(!v.key({'x', false, false, false}).close);
    EXPECT(v.key({keys::Escape, false, false, false}).close);
    EXPECT(ShortcutsView(buildCommandRegistry(), true, true).key({'q', false, false, false}).close);
    // Without desktop keys only prefix ones are listed.
    ShortcutsView prefixOnly(buildCommandRegistry(), false, true);
    for (const auto &row : shortcutRows(buildCommandRegistry(), false, true)) {
        EXPECT(row.desktop.empty());
    }
    // A tiny window never breaks it.
    for (int c = 1; c <= 20; ++c) {
        for (int r = 1; r <= 8; ++r) {
            ShortcutsView s(buildCommandRegistry(), true, true);
            const auto sz = s.wanted(c, r);
            s.resize(sz.columns, sz.rows);
            CellBuffer grid(std::size_t(sz.columns), std::size_t(sz.rows));
            CursorState cs;
            s.render(grid, cs);
            s.key({keys::PageDown, false, false, false});
            s.render(grid, cs);
        }
    }
}

TEST_CASE("The which-key box lists the keys after the prefix, one line per command") {
    const auto lines = whichKeyLines(PrefixKeys::Config{});
    auto has = [&](const std::string &text) {
        for (const auto &l : lines) {
            if (l == text) return true;
        }
        return false;
    };
    EXPECT(has("c  New tab"));
    EXPECT(has("1-0  Go to tab"));
    EXPECT(has("?  Keyboard shortcuts"));
    EXPECT(has("%  Split side by side"));
    WhichKey box{PrefixKeys::Config{}};
    const auto size = box.wanted(80, 24);
    box.resize(size.columns, size.rows);
    CellBuffer cells(std::size_t(size.columns), std::size_t(size.rows));
    CursorState cursor;
    box.render(cells, cursor);
    EXPECT(rowText(cells, 1).find("Ctrl+B then") != std::string::npos);
    EXPECT(rowText(cells, 2).find("New tab") != std::string::npos);
    for (int c = 1; c <= 20; ++c) {
        for (int r = 1; r <= 8; ++r) {
            WhichKey w{PrefixKeys::Config{}};
            const auto sz = w.wanted(c, r);
            w.resize(sz.columns, sz.rows);
            CellBuffer grid(std::size_t(sz.columns), std::size_t(sz.rows));
            CursorState cs;
            w.render(grid, cs);
        }
    }
}

TEST_SUIT_END
