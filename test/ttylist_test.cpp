#include "mls-unit-test/unittest.h"
#include "tty/canvas.h"
#include "ttylist.h"

#include <string>
#include <vector>

namespace {
using namespace frontend::ttymode;

mtd::SessionEntry entry(const std::string &id,
                        bool attached = false,
                        const std::string &name = {}) {
    mtd::SessionEntry e;
    e.id = id;
    e.attached = attached;
    e.name = name;
    return e;
}

mtd::SessionInfo info(const std::string &cwd,
                      bool busy = false,
                      std::vector<std::string> command = {}) {
    mtd::SessionInfo i;
    i.cwd = cwd;
    i.busy = busy;
    i.command = std::move(command);
    return i;
}

std::string rowText(const ::tty::Canvas &image, int y) {
    std::string out;
    for (int x = 0; x < image.columns(); ++x) {
        const char32_t cp = image.at(x, y).codePoint;
        out += cp < 128 ? char(cp) : '?';
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

::tty::Canvas drawn(const ListModel &model, int columns, int rows) {
    ::tty::Canvas canvas(columns, rows);
    renderList(model, canvas);
    return canvas;
}

ListModel threeRows() {
    ListModel model;
    model.setEntries(
        {makeListEntry(entry("aaaaaaaa11", false, "build"), nullptr),
         makeListEntry(entry("bbbbbbbb22", true), nullptr),
         makeListEntry(entry("cccccccc33"), nullptr)});
    return model;
}
} // namespace

TEST_SUIT_BEGIN(TtyList)

TEST_CASE("A row says the name, what runs and whether another client has it") {
    const auto shell = info("/home/me/src");
    const auto vim = info("/home/me", true, {"vim", "notes.txt"});
    const ListEntry a =
        makeListEntry(entry("0123456789abcdef", false, "editor"), &vim);
    EXPECT_EQ(a.title, std::string("editor"));
    EXPECT_EQ(a.what, std::string("vim notes.txt"));
    EXPECT(!a.inUse);
    const ListEntry b = makeListEntry(entry("0123456789abcdef", true), &shell);
    EXPECT_EQ(b.title, std::string("terminal 01234567"));
    EXPECT_EQ(b.what, std::string("shell in /home/me/src"));
    EXPECT(b.inUse);
    const ListEntry c = makeListEntry(entry("x"), nullptr);
    EXPECT_EQ(c.what, std::string());
}

TEST_CASE("The name of the session info is used when the list has none") {
    mtd::SessionInfo named = info("/tmp");
    named.name = "laptop";
    EXPECT_EQ(makeListEntry(entry("abc"), &named).title, std::string("laptop"));
}

TEST_CASE("Control characters in a command line never reach the screen") {
    const auto evil = info("/", true, {"echo", "\x1b[2J\x07hi"});
    const ListEntry e = makeListEntry(entry("abc"), &evil);
    for (const char c : e.what) {
        EXPECT(static_cast<unsigned char>(c) >= 32 && c != 127);
    }
}

TEST_CASE(
    "Up, down, first, last and digits move the chosen row inside the list") {
    ListModel model = threeRows();
    EXPECT_EQ(model.index(), std::size_t(0));
    model.key(ListKey::Up);
    EXPECT_EQ(model.index(), std::size_t(0));
    model.key(ListKey::Down);
    model.key(ListKey::Down);
    model.key(ListKey::Down);
    EXPECT_EQ(model.index(), std::size_t(2));
    model.key(ListKey::First);
    EXPECT_EQ(model.index(), std::size_t(0));
    model.key(ListKey::Last);
    EXPECT_EQ(model.index(), std::size_t(2));
    model.key(ListKey::Digit, 2);
    EXPECT_EQ(model.index(), std::size_t(1));
    model.key(ListKey::Digit, 9); // No ninth row: stays.
    EXPECT_EQ(model.index(), std::size_t(1));
}

TEST_CASE("Enter opens the chosen terminal, even when it is the only one") {
    ListModel model;
    EXPECT(model.key(ListKey::Open).kind ==
           ListAction::Kind::None); // Nothing to open.
    model.setEntries({makeListEntry(entry("only"), nullptr)});
    const auto action = model.key(ListKey::Open);
    EXPECT(action.kind == ListAction::Kind::Open);
    EXPECT_EQ(action.id, std::string("only"));
    EXPECT(model.key(ListKey::Quit).kind == ListAction::Kind::Quit);
    EXPECT(model.key(ListKey::Refresh).kind == ListAction::Kind::Refresh);
}

TEST_CASE("A refresh keeps the chosen terminal when it is still there") {
    ListModel model = threeRows();
    model.key(ListKey::Down);
    model.setEntries(
        {makeListEntry(entry("new1"), nullptr),
         makeListEntry(entry("aaaaaaaa11", false, "build"), nullptr),
         makeListEntry(entry("bbbbbbbb22", true), nullptr)});
    EXPECT_EQ(model.selected()->id, std::string("bbbbbbbb22"));
    model.setEntries(
        {makeListEntry(entry("zzz"), nullptr)}); // Gone: back to the first.
    EXPECT_EQ(model.index(), std::size_t(0));
}

TEST_CASE("Keys of the host terminal map to list keys") {
    HostInputDecoder decoder;
    int digit = 0;
    const auto map = [&](const std::string &bytes) {
        auto events = decoder.feed(bytes);
        for (auto &e : decoder.flush()) {
            events.push_back(e);
        }
        return events.size() == 1 ? listKeyOf(events[0], digit) : ListKey::None;
    };
    EXPECT(map("\x1b[A") == ListKey::Up);
    EXPECT(map("\x1b[B") == ListKey::Down);
    EXPECT(map("\r") == ListKey::Open);
    EXPECT(map("q") == ListKey::Quit);
    EXPECT(map("\x1b") == ListKey::Quit);
    EXPECT(map("\x03") == ListKey::Quit);
    EXPECT(map("\x1d") == ListKey::Quit);
    EXPECT(map("j") == ListKey::Down);
    EXPECT(map("k") == ListKey::Up);
    EXPECT(map("r") == ListKey::Refresh);
    EXPECT(map("3") == ListKey::Digit && digit == 3);
    EXPECT(map("x") == ListKey::None);
}

TEST_CASE("The list is drawn with a header, the rows, the mark for in use and "
          "a footer") {
    const ListModel model = threeRows();
    const ::tty::Canvas image = drawn(model, 70, 10);
    EXPECT_EQ(rowText(image, 0), std::string(" matterm: running terminals"));
    EXPECT(rowText(image, 2).find("1 build") != std::string::npos);
    EXPECT(rowText(image, 3).find("2 terminal bbbbbbbb") != std::string::npos);
    EXPECT(rowText(image, 3).find("(in use)") != std::string::npos);
    EXPECT(rowText(image, 2).find("(in use)") == std::string::npos);
    EXPECT(rowText(image, 8).empty()); // The chosen row is free: no remark.
    EXPECT(rowText(image, 9).find("q quit") != std::string::npos);
    // The chosen row is drawn inverted: dark on light.
    EXPECT(image.at(5, 2).background.r > image.at(5, 3).background.r);
}

TEST_CASE(
    "The mark for in use is seen on a narrow screen with a long folder name") {
    ListModel model;
    mtd::SessionInfo far =
        info("/home/someone/projects/a/very/deep/folder/tree/that/goes/on");
    model.setEntries({makeListEntry(entry("aaaaaaaa11", true), &far)});
    const ::tty::Canvas image = drawn(model, 40, 8);
    EXPECT(rowText(image, 2).find("(in use)") != std::string::npos);
    EXPECT(rowText(image, 6).find("opening takes it over") !=
           std::string::npos);
}

TEST_CASE("An empty list says so and still has its footer") {
    ListModel model;
    const ::tty::Canvas image = drawn(model, 60, 8);
    EXPECT(rowText(image, 2).find("No terminals are running") !=
           std::string::npos);
    EXPECT(rowText(image, 7).find("q quit") != std::string::npos);
}

TEST_CASE("A message shows under the list") {
    ListModel model = threeRows();
    model.setMessage("The daemon is not running");
    EXPECT(rowText(drawn(model, 60, 10), 8).find("not running") !=
           std::string::npos);
}

TEST_CASE("A long list scrolls to keep the chosen row visible, and tiny "
          "screens do not crash") {
    ListModel model;
    std::vector<ListEntry> many;
    for (int i = 0; i < 30; ++i) {
        many.push_back(makeListEntry(entry("id" + std::to_string(100 + i),
                                           false,
                                           "t" + std::to_string(i)),
                                     nullptr));
    }
    model.setEntries(many);
    for (int i = 0; i < 20; ++i) {
        model.key(ListKey::Down);
    }
    const ::tty::Canvas image = drawn(model, 40, 10);
    bool found = false;
    for (int y = 0; y < 10; ++y) {
        found = found || rowText(image, y).find("t20") != std::string::npos;
    }
    EXPECT(found);
    for (int c = 0; c <= 3; ++c) {
        for (int r = 0; r <= 3; ++r) {
            drawn(model, c, r); // Must not throw or write outside.
        }
    }
}

TEST_SUIT_END
