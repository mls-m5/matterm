#include "listmetadata.h"
#include "mls-unit-test/unittest.h"
#include "palette.h"
#include "terminalmodel.h"
#include "tty/canvas.h"
#include "ttylist.h"
#include "vtparser.h"

#include <limits>
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
    canvas.setWideChars(true);
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
    EXPECT_EQ(b.title, std::string("src")); // No name: the top folder.
    EXPECT_EQ(b.what, std::string("shell"));
    EXPECT_EQ(b.folder, std::string("/home/me/src"));
    EXPECT(b.inUse);
    // The daemon did not answer: only the id is known.
    const ListEntry c = makeListEntry(entry("0123456789abcdef"), nullptr);
    EXPECT_EQ(c.title, std::string("terminal 01234567"));
    EXPECT_EQ(c.what, std::string());
}

TEST_CASE(
    "The first column is the explicit name, else the name of the top folder") {
    const auto in = [](const std::string &cwd) { return info(cwd); };
    EXPECT_EQ(folderName("/home/a/projects/matterm"), std::string("matterm"));
    EXPECT_EQ(folderName("/home/a/projects/matterm/"), std::string("matterm"));
    EXPECT_EQ(folderName("/home"), std::string("home"));
    EXPECT_EQ(folderName("/"), std::string("/"));
    EXPECT_EQ(folderName("//"), std::string("/"));
    EXPECT_EQ(folderName(""), std::string("/"));
    EXPECT_EQ(folderName("relative/dir"), std::string("dir"));
    EXPECT_EQ(folderName("/tmp/a\x1b[31mb"),
              std::string("a [31mb")); // No control characters.
    auto root = in("/");
    EXPECT_EQ(makeListEntry(entry("abc"), &root).title, std::string("/"));
    auto none = in("");
    EXPECT_EQ(makeListEntry(entry("abc"), &none).title, std::string("/"));
    auto deep = in("/home/a/projects/matterm");
    EXPECT_EQ(makeListEntry(entry("abc"), &deep).title, std::string("matterm"));
    // A name given by --name or Rename terminal... wins, from the list entry or
    // from the info.
    EXPECT_EQ(makeListEntry(entry("abc", false, "work"), &deep).title,
              std::string("work"));
    mtd::SessionInfo named = in("/home/a/projects/matterm");
    named.name = "laptop";
    EXPECT_EQ(makeListEntry(entry("abc"), &named).title, std::string("laptop"));
}

TEST_CASE(
    "Each terminal shows its name and program above its folder and metadata") {
    ListModel model;
    auto vim = info("/home/a/projects/matterm", true, {"vim", "x.cpp"});
    auto sh = info("/srv/www");
    model.setEntries({makeListEntry(entry("aaaaaaaa11"), &vim),
                      makeListEntry(entry("bbbbbbbb22"), &sh)});
    const ::tty::Canvas image = drawn(model, 100, 13);
    EXPECT(rowText(image, 5).find("1 matterm  vim x.cpp") != std::string::npos);
    EXPECT(rowText(image, 6).find("/home/a/projects/matterm") !=
           std::string::npos);
    EXPECT(rowText(image, 8).find("2 www  shell") != std::string::npos);
    EXPECT(rowText(image, 9).find("/srv/www") != std::string::npos);
}

TEST_CASE(
    "With preferNew the list starts on New terminal, also after a refresh") {
    ListModel model;
    model.setPreferNew(true);
    model.setEntries({makeListEntry(entry("a"), nullptr),
                      makeListEntry(entry("b"), nullptr)});
    EXPECT(model.newSelected());
    EXPECT(model.key(ListKey::Open).kind == ListAction::Kind::New);
    model.setEntries({makeListEntry(entry("a"), nullptr),
                      makeListEntry(entry("b"), nullptr),
                      makeListEntry(entry("c"), nullptr)});
    EXPECT(model.newSelected()); // Still: nothing else was chosen.
    model.key(ListKey::Down);
    model.setEntries({makeListEntry(entry("a"), nullptr),
                      makeListEntry(entry("b"), nullptr)});
    EXPECT_EQ(model.selected()->id,
              std::string("a")); // A chosen terminal is kept.
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
    EXPECT_EQ(model.index(),
              std::size_t(1)); // The first terminal, not "New terminal".
    model.key(ListKey::Up);
    EXPECT(model.newSelected());
    model.key(ListKey::Up);
    EXPECT(model.newSelected());
    for (int i = 0; i < 5; ++i) {
        model.key(ListKey::Down);
    }
    EXPECT_EQ(model.index(), std::size_t(3));
    model.key(ListKey::First);
    EXPECT_EQ(model.index(), std::size_t(0));
    model.key(ListKey::Last);
    EXPECT_EQ(model.index(), std::size_t(3));
    model.key(ListKey::Digit, 2);
    EXPECT_EQ(model.index(), std::size_t(2));
    model.key(ListKey::Digit, 9); // No ninth terminal: stays.
    EXPECT_EQ(model.index(), std::size_t(2));
}

TEST_CASE("Enter opens the chosen terminal, even when it is the only one") {
    ListModel model;
    EXPECT(model.key(ListKey::Open).kind ==
           ListAction::Kind::New); // Nothing else to open: a new terminal.
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
    EXPECT_EQ(model.index(), std::size_t(1));
}

TEST_CASE("\"New terminal\" is the first row, also with n; the terminal this "
          "program runs in is refused") {
    ListModel model = threeRows();
    model.key(ListKey::First);
    EXPECT(model.key(ListKey::Open).kind == ListAction::Kind::New);
    EXPECT(model.key(ListKey::New).kind == ListAction::Kind::New);
    ListModel own;
    own.setEntries({makeListEntry(entry("mine"), nullptr, "mine"),
                    makeListEntry(entry("other"), nullptr, "mine")});
    EXPECT(own.entries()[0].thisTerminal && !own.entries()[1].thisTerminal);
    EXPECT(own.key(ListKey::Open).kind ==
           ListAction::Kind::None); // The chosen one is the first terminal.
    EXPECT(own.message().find("feed it to itself") != std::string::npos);
    own.key(ListKey::Down);
    EXPECT(own.key(ListKey::Open).kind == ListAction::Kind::Open);
    const ::tty::Canvas image = drawn(own, 70, 10);
    EXPECT(rowText(image, 2).find("(this terminal)") != std::string::npos);
}

TEST_CASE("x asks before a terminal is ended: the question names it and what "
          "runs, the default is to keep it") {
    ListModel model;
    auto vim = info("/home/a/projects/matterm", true, {"vim", "x.cpp"});
    model.setEntries({makeListEntry(entry("aaaaaaaa11"), &vim),
                      makeListEntry(entry("bbbbbbbb22", true), nullptr)});
    EXPECT(model.key(ListKey::End).kind ==
           ListAction::Kind::None); // Only asks.
    EXPECT(model.confirming());
    EXPECT_EQ(
        model.question(),
        std::string("End \"matterm\" (vim x.cpp)? y = end, n/Esc = keep"));
    model.key(ListKey::Open); // Enter keeps it.
    EXPECT(!model.confirming());
    // Escape, n, anything but y keeps it; the question is gone.
    for (const ListKey no : {ListKey::Open,
                             ListKey::Quit,
                             ListKey::New,
                             ListKey::Down,
                             ListKey::None}) {
        EXPECT(model.key(ListKey::End).kind == ListAction::Kind::None);
        EXPECT(model.confirming());
        const auto action = model.key(no);
        EXPECT(action.kind == ListAction::Kind::None);
        EXPECT(!model.confirming() && model.question().empty());
    }
    EXPECT_EQ(model.entries().size(), std::size_t(2));
    model.key(ListKey::End);
    const auto yes = model.key(ListKey::Yes);
    EXPECT(yes.kind == ListAction::Kind::End);
    EXPECT_EQ(yes.id, std::string("aaaaaaaa11"));
    EXPECT_EQ(yes.title, std::string("matterm"));
    EXPECT(!model.confirming());
    EXPECT(model.key(ListKey::Yes).kind ==
           ListAction::Kind::None); // y without a question does nothing.
}

TEST_CASE("The question says when another client has the terminal; the own "
          "terminal and New terminal cannot be ended") {
    ListModel model;
    auto sh = info("/srv/www");
    auto taken = makeListEntry(entry("bbbbbbbb22", true), &sh);
    model.setEntries({makeListEntry(entry("mine"), &sh, "mine"), taken});
    model.key(
        ListKey::End); // The chosen row is the first terminal: the own one.
    EXPECT(!model.confirming());
    EXPECT(model.message().find("end this program too") != std::string::npos);
    model.key(ListKey::Down);
    model.key(ListKey::End);
    EXPECT(model.confirming());
    EXPECT(model.question().find("(also open in another client)") !=
           std::string::npos);
    EXPECT(model.question().find("y = end, n/Esc = keep") != std::string::npos);
}

TEST_CASE("After a terminal was ended the chosen row stays near where it was") {
    ListModel model;
    std::vector<ListEntry> three = {makeListEntry(entry("a"), nullptr),
                                    makeListEntry(entry("b"), nullptr),
                                    makeListEntry(entry("c"), nullptr)};
    model.setEntries(three);
    model.key(ListKey::Down); // b
    model.key(ListKey::End);
    model.key(ListKey::Yes);
    model.setEntries({three[0], three[2]});            // b is gone.
    EXPECT_EQ(model.selected()->id, std::string("c")); // The next one.
    model.key(ListKey::Down); // Last row (c is at row 2).
    model.key(ListKey::End);
    model.key(ListKey::Yes);
    model.setEntries({three[0]});
    EXPECT_EQ(model.selected()->id,
              std::string("a")); // The last one was ended: the one before.
    model.key(ListKey::End);
    model.key(ListKey::Yes);
    model.setEntries({});
    EXPECT(model.newSelected()); // None left.
}

TEST_CASE(
    "The question is on screen in the note line, and the footer names x") {
    ListModel model;
    auto vim = info("/home/a/projects/matterm", true, {"vim"});
    model.setEntries({makeListEntry(entry("a"), &vim)});
    model.key(ListKey::End);
    const ::tty::Canvas image = drawn(model, 80, 10);
    EXPECT(rowText(image, 8).find(
               "End \"matterm\" (vim)? y = end, n/Esc = keep") !=
           std::string::npos);
    EXPECT(rowText(image, 9).find("x end") != std::string::npos);
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
    EXPECT(map("n") == ListKey::New);
    EXPECT(map("x") == ListKey::End);
    EXPECT(map("\x1b[3~") == ListKey::End); // Delete.
    EXPECT(map("y") == ListKey::Yes);
    EXPECT(map("3") == ListKey::Digit && digit == 3);
    EXPECT(map("w") == ListKey::None);
}

TEST_CASE("The list is drawn with a header, the rows, the mark for in use and "
          "a footer") {
    const ListModel model = threeRows();
    const ::tty::Canvas image = drawn(model, 70, 16);
    EXPECT_EQ(rowText(image, 0), std::string(" matterm: running terminals"));
    EXPECT(rowText(image, 2).find("n New terminal") != std::string::npos);
    EXPECT(rowText(image, 5).find("1 build") != std::string::npos);
    EXPECT(rowText(image, 8).find("2 terminal bbbbbbbb") != std::string::npos);
    EXPECT(rowText(image, 8).find("(in use)") != std::string::npos);
    EXPECT(rowText(image, 5).find("(in use)") == std::string::npos);
    EXPECT(rowText(image, 14).empty()); // The chosen terminal is free.
    EXPECT(rowText(image, 15).find("q quit") != std::string::npos);
    EXPECT(image.at(5, 5).inverse);
    EXPECT(image.at(5, 5).background.r > image.at(5, 8).background.r);
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
    EXPECT(rowText(image, 3).find("No terminals are running") !=
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

TEST_CASE("A selected terminal is a three-line block with background and "
          "reverse plus a marker") {
    ListModel model = threeRows();
    const auto image = drawn(model, 80, 10);
    EXPECT_EQ(image.at(0, 5).codePoint, U'>');
    for (int y = 5; y < 8; ++y) {
        for (int x = 0; x < 80; ++x) {
            EXPECT(image.at(x, y)
                       .inverse); // Monochrome cue independent of colour.
            EXPECT(image.at(x, y).background != kScreenBg);
            EXPECT(image.at(x, y).sameStyle(image.at(x, 5)) ||
                   image.at(x, y).bold != image.at(x, 5).bold);
        }
    }
    // Even after discarding all attributes the persistent marker remains.
    EXPECT(rowText(image, 5).find('>') != std::string::npos);
    EXPECT(rowText(image, 6).find("unknown") != std::string::npos);
    EXPECT(rowText(image, 7).find("preview unavailable") != std::string::npos);
}

TEST_CASE("Paging counts entries, and tiny heights never hide the selected "
          "terminal") {
    EXPECT_EQ(listCapacity(10), 2);
    EXPECT_EQ(listCapacity(13), 3);
    EXPECT_EQ(listCapacity(6), 1);
    ListModel model = threeRows();
    model.key(ListKey::PageDown, 0, listCapacity(10));
    EXPECT_EQ(model.index(), std::size_t(3));
    model.key(ListKey::PageUp, 0, listCapacity(10));
    EXPECT_EQ(model.index(), std::size_t(1));
    model.key(ListKey::Last);
    for (int height = 1; height <= 13; ++height) {
        const auto image = drawn(model, 80, height);
        bool marker = false, title = false;
        for (int y = 0; y < height; ++y) {
            marker = marker || image.at(0, y).codePoint == U'>';
            title = title || rowText(image, y).find("terminal cccccccc") !=
                                 std::string::npos;
        }
        EXPECT(marker && title);
    }
}

TEST_CASE(
    "Optional metadata separates shell foreground, output age and attachment") {
    auto shell = info("/tmp/work");
    auto e = makeListEntry(entry("a", true, "work"), &shell);
    e.foregroundBusy =
        false; // Explicit verified detail, not the legacy busy bit.
    e.outputAgeMs = 2500;
    e.preview = "still building";
    ListModel model;
    model.setEntries({e});
    const auto image = drawn(model, 100, 10);
    EXPECT(rowText(image, 6).find("shell foreground") != std::string::npos);
    EXPECT(rowText(image, 6).find("attached") != std::string::npos);
    EXPECT(rowText(image, 6).find("2s ago") != std::string::npos);
    EXPECT(rowText(image, 6).find("idle") == std::string::npos);
    EXPECT(rowText(image, 7).find("still building") != std::string::npos);
    model.setPreviewEnabled(false);
    EXPECT(rowText(drawn(model, 100, 10), 7).find("preview disabled") !=
           std::string::npos);
}

TEST_CASE("Preview rendering is bounded, control-safe and clipped by terminal "
          "cell width") {
    ListModel model;
    ListEntry e = makeListEntry(entry("a", false, "wide"), nullptr);
    e.preview = "\x1b]52;c;secret\x07\r\n" + std::string(1000000, 'x');
    model.setEntries({e});
    auto image = drawn(model, 24, 10);
    const std::string wire = image.flush();
    EXPECT(wire.find("\x1b]52") == std::string::npos);
    EXPECT(wire.find("secret") ==
           std::string::npos); // Do not reveal an OSC payload by merely
                               // stripping ESC.
    EXPECT(rowText(image, 7).find("unsafe preview") != std::string::npos);
    for (int x = 0; x < image.columns(); ++x) {
        const auto cp = image.at(x, 7).codePoint;
        EXPECT(cp >= 32 && !(cp >= 127 && cp <= 159));
    }
    e.preview = "界界界界界界界界界界界界界界";
    model.setEntries({e});
    image = drawn(model, 15, 10);
    // Preview indentation consumes four columns; a wide glyph must never
    // begin in the final column or overwrite the cell after its allocation.
    EXPECT(image.at(14, 7).codePoint != U'界');
}

TEST_CASE("Selected backgrounds stay light after real SGR inversion in "
          "truecolor and ANSI mono fallback") {
    const ListModel list = threeRows();
    for (const bool truecolor : {true, false}) {
        ::tty::Caps caps;
        caps.truecolor = truecolor;
        caps.colors256 = false;
        ::tty::Canvas image(80, 10, caps);
        renderList(list, image);
        TerminalModel terminal(80, 10, 0);
        VtParser parser(terminal);
        parser.feed(image.flush(true));
        const auto frame = terminal.frame(16);
        Palette palette;
        for (std::size_t y = 5; y < 8; ++y) {
            for (std::size_t x = 0; x < 80; ++x) {
                const auto effective =
                    palette.resolve(frame.cells.row(y).data()[x]);
                EXPECT(effective.background.r > effective.foreground.r);
                EXPECT(effective.background.g > effective.foreground.g);
                EXPECT(effective.background.b > effective.foreground.b);
            }
        }
    }
}

TEST_CASE("Legacy busy false does not prove a foreground shell; verified "
          "metadata may") {
    const auto legacyInfo = info("/tmp/work", false);
    auto e = makeListEntry(entry("legacy", false, "work"), &legacyInfo);
    EXPECT(!e.foregroundBusy.has_value());
    ListModel list;
    list.setEntries({e});
    EXPECT(rowText(drawn(list, 100, 10), 6).find("foreground unknown") !=
           std::string::npos);
    e.foregroundBusy =
        false; // Only explicit, verified new metadata may set this.
    list.setEntries({e});
    EXPECT(rowText(drawn(list, 100, 10), 6).find("shell foreground") !=
           std::string::npos);
}

TEST_CASE("A scroll delta may omit title characters; a resized full snapshot "
          "repeats the name") {
    ListModel list;
    list.setEntries({makeListEntry(entry("a", false, "matterm-demo"), nullptr),
                     makeListEntry(entry("b", false, "Namedone"), nullptr)});
    ::tty::Canvas canvas(80, 8); // Exactly one terminal block fits.
    renderList(list, canvas);
    TerminalModel terminal(80, 8, 0);
    VtParser parser(terminal);
    parser.feed(canvas.flush(true));
    list.key(ListKey::Down);
    renderList(list, canvas);
    const std::string delta = canvas.flush();
    // Both names have 'a' in the same column/style. Canvas correctly omits
    // that unchanged cell: searching stripped raw output for Namedone fails.
    EXPECT(delta.find("Namedone") == std::string::npos);
    parser.feed(delta);
    const auto frame = terminal.frame(16);
    std::string visible;
    for (std::size_t x = 4; x < 12; ++x) {
        ::tty::appendUtf8(visible, frame.cells.row(2).data()[x].codePoint);
    }
    EXPECT_EQ(visible, std::string("Namedone"));
    canvas.resize(81, 8);
    renderList(list, canvas);
    EXPECT(canvas.flush().find("Namedone") != std::string::npos);
}

TEST_CASE(
    "Details batches advance by processed prefix including vanished IDs") {
    std::vector<mtd::SessionEntry> fixtures;
    for (int i = 0; i < 20; ++i)
        fixtures.push_back(entry(std::string(30, 'a') + char('a' + i / 16) +
                                 "0123456789abcdef"[i % 16]));
    ListMetadataTools tools;
    tools.nowMs = [] { return std::int64_t(0); };
    int calls = 0;
    bool validRequests = true;
    tools.ask = [&](const mtd::ControlRequest &request,
                    int timeout) -> std::optional<mtd::ControlReply> {
        validRequests = validRequests && timeout == 500;
        const auto &q = std::get<mtd::GetSessionDetails>(request);
        validRequests = validRequests && q.includePreview;
        mtd::SessionDetails reply;
        if (++calls == 1) {
            validRequests = validRequests && q.ids.size() == 16;
            reply.processedCount = 3;
            for (const auto index : {0u, 2u}) {
                mtd::SessionDetail d;
                d.id = q.ids[index];
                d.cwd = "/tmp/work";
                d.foregroundBusy = false;
                d.outputAgeMs = 123;
                d.preview = "ready";
                reply.rows.push_back(d);
            }
        }
        else {
            validRequests = validRequests &&
                            q.ids.front() == fixtures[calls == 2 ? 3 : 19].id;
            reply.processedCount = std::uint16_t(q.ids.size());
        }
        return reply;
    };
    const auto result =
        readListMetadata(fixtures, {1, 3, 5, {}}, true, {}, tools);
    EXPECT_EQ(calls, 3);
    EXPECT(validRequests);
    EXPECT_EQ(result.size(), fixtures.size());
    EXPECT(result[0].foregroundBusy.has_value() && !*result[0].foregroundBusy);
    EXPECT_EQ(*result[0].preview, std::string("ready"));
    EXPECT_EQ(*result[0].outputAgeMs, std::uint64_t(123));
    EXPECT(!result[1].preview && !result[1].foregroundBusy); // Vanished.
}

TEST_CASE(
    "Details reject zero progress, foreign prefix IDs and duplicate rows") {
    const std::vector<mtd::SessionEntry> fixtures = {
        entry(std::string(32, 'a')), entry(std::string(32, 'b'))};
    for (int mode = 0; mode < 3; ++mode) {
        ListMetadataTools tools;
        tools.nowMs = [] { return std::int64_t(0); };
        int calls = 0;
        tools.ask = [&](const mtd::ControlRequest &,
                        int) -> std::optional<mtd::ControlReply> {
            ++calls;
            mtd::SessionDetails reply;
            reply.processedCount = mode == 0 ? 0 : 1;
            mtd::SessionDetail d;
            d.id = fixtures[mode == 1 ? 1 : 0].id;
            d.preview = "must not leak";
            if (mode)
                reply.rows.push_back(d);
            if (mode == 2)
                reply.rows.push_back(d);
            return reply;
        };
        const auto result =
            readListMetadata(fixtures, {1, 3, 5, {}}, true, {}, tools);
        EXPECT_EQ(calls, 1);
        EXPECT(!result[0].preview && !result[1].preview);
    }
}

TEST_CASE(
    "Legacy details are version gated and share one bounded request budget") {
    const std::vector<mtd::SessionEntry> fixtures = {
        entry(std::string(32, 'a')), entry(std::string(32, 'b'))};
    ListMetadataTools tools;
    std::int64_t now = 0;
    tools.nowMs = [&] { return now; };
    int calls = 0;
    bool bounded = true;
    tools.ask = [&](const mtd::ControlRequest &request,
                    int timeout) -> std::optional<mtd::ControlReply> {
        bounded = bounded && timeout <= 500;
        ++calls;
        const auto &q =
            std::get<mtd::GetSessionInfo>(request); // Never details on 1.3.4.
        now += 1500;
        mtd::SessionInfo reply;
        reply.id = q.id;
        reply.cwd = "/tmp/legacy";
        reply.busy = false;
        return reply;
    };
    const auto result =
        readListMetadata(fixtures, {1, 3, 4, {}}, true, {}, tools);
    EXPECT_EQ(calls, 1);
    EXPECT(bounded);
    EXPECT_EQ(result[0].title, std::string("legacy"));
    EXPECT(!result[0].foregroundBusy && !result[0].preview &&
           !result[0].outputAgeMs);
    EXPECT(result[1].folder.empty());
}

TEST_CASE(
    "Disabled or invalid preview metadata is never retained by the client") {
    const std::vector<mtd::SessionEntry> fixtures = {
        entry(std::string(32, 'a'))};
    for (const bool enabled : {true, false}) {
        ListMetadataTools tools;
        tools.nowMs = [] { return std::int64_t(0); };
        bool requestedCorrectly = false;
        tools.ask = [&](const mtd::ControlRequest &request,
                        int) -> std::optional<mtd::ControlReply> {
            requestedCorrectly =
                std::get<mtd::GetSessionDetails>(request).includePreview ==
                enabled;
            mtd::SessionDetail d;
            d.id = fixtures[0].id;
            d.preview = enabled ? "\x1b]52;c;secret\x07" : "not requested";
            return mtd::SessionDetails{1, {d}};
        };
        const auto result =
            readListMetadata(fixtures, {1, 3, 5, {}}, enabled, {}, tools);
        EXPECT(requestedCorrectly);
        EXPECT(!result[0].preview);
    }
}

TEST_CASE("Disabling previews clears cached text and filters later stale "
          "refresh data") {
    auto e = makeListEntry(entry("a"), nullptr);
    e.preview = "private";
    ListModel model;
    model.setEntries({e});
    model.setPreviewEnabled(false);
    EXPECT(!model.entries()[0].preview);
    model.setEntries(
        {e}); // Defense in depth even if a source returns stale text.
    EXPECT(!model.entries()[0].preview);
    model.setPreviewEnabled(true);
    EXPECT(!model.entries()[0].preview);
}

TEST_CASE("A standard-width metadata row reserves space for explicit unknown "
          "output age") {
    const auto legacy = info("/home/user/projects/a-long-folder-tree", false);
    ListModel model;
    model.setEntries({makeListEntry(entry("a", false, "work"), &legacy)});
    const auto image = drawn(model, 80, 10);
    EXPECT(rowText(image, 6).find("foreground unknown") != std::string::npos);
    EXPECT(rowText(image, 6).find("not attached") != std::string::npos);
    EXPECT(rowText(image, 6).find("output unknown") != std::string::npos);
}

TEST_CASE("Known output samples age on the local monotonic clock without "
          "claiming live inactivity") {
    auto e = makeListEntry(entry("a", false, "work"), nullptr);
    e.outputAgeMs = 2000;
    ListModel model;
    model.setNowMs(1000);
    model.setEntries({e});
    EXPECT(rowText(drawn(model, 100, 10), 6).find("known output 2s ago") !=
           std::string::npos);
    model.setNowMs(31000);
    const auto later = rowText(drawn(model, 100, 10), 6);
    EXPECT(later.find("known output 32s ago") != std::string::npos);
    EXPECT(later.find("output 2s ago") == std::string::npos);
    e.outputAgeMs.reset();
    model.setEntries({e});
    model.setNowMs(61000);
    EXPECT(rowText(drawn(model, 100, 10), 6).find("output unknown") !=
           std::string::npos);
}

TEST_CASE("Local output sample age saturates instead of wrapping and clamps a "
          "backward clock") {
    ListEntry e;
    e.outputAgeMs = std::numeric_limits<std::uint64_t>::max() - 1000;
    e.outputAgeMeasuredAtMs = 1000;
    EXPECT_EQ(*knownOutputAgeMs(e, 31000),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(*knownOutputAgeMs(e, 500), *e.outputAgeMs);
    e.outputAgeMs.reset();
    EXPECT(!knownOutputAgeMs(e, 31000));
}

TEST_SUIT_END
