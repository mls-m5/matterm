#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "tty/matterm/snapshot.h"
#include "vtparser.h"

#include <string>

namespace {
struct Term {
    TerminalModel model;
    VtParser parser;
    explicit Term(std::size_t columns = 20,
                  std::size_t rows = 6,
                  std::size_t scrollback = 200)
        : model(columns, rows, scrollback)
        , parser(model) {}
    void feed(const std::string &bytes) {
        parser.feed(bytes);
    }
};

bool sameColor(const ColorReference &a, const ColorReference &b) {
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == ColorReference::Kind::Default) {
        return true;
    }
    if (a.kind == ColorReference::Kind::Indexed) {
        return a.value.r == b.value.r;
    }
    return a.value.r == b.value.r && a.value.g == b.value.g &&
           a.value.b == b.value.b;
}

bool sameCell(const FChar &a, const FChar &b) {
    return a.codePoint == b.codePoint && a.style() == b.style() &&
           sameColor(a.foregroundColor(), b.foregroundColor()) &&
           sameColor(a.backgroundColor(), b.backgroundColor());
}

// What differs between two terminals, or "" when they are the same: the screen,
// the cursor, the saved lines, the margins and the input modes.
std::string difference(Term &a, Term &b, bool history = true) {
    if (a.model.columns() != b.model.columns() ||
        a.model.rows() != b.model.rows()) {
        return "size";
    }
    if (a.model.alternateScreen() != b.model.alternateScreen()) {
        return "alternate screen";
    }
    for (std::size_t y = 0; y < a.model.rows(); ++y) {
        for (std::size_t x = 0; x < a.model.columns(); ++x) {
            if (!sameCell(a.model.cell(x, y), b.model.cell(x, y))) {
                return "cell " + std::to_string(x) + "," + std::to_string(y);
            }
        }
    }
    const auto ca = a.model.cursor(), cb = b.model.cursor();
    if (ca.column != cb.column || ca.row != cb.row) {
        return "cursor position " + std::to_string(cb.column) + "," +
               std::to_string(cb.row);
    }
    if (ca.visible != cb.visible) {
        return "cursor visibility";
    }
    if (ca.shape != cb.shape || ca.blink != cb.blink) {
        return "cursor shape";
    }
    if (a.model.scrollTop() != b.model.scrollTop() ||
        a.model.scrollBottom() != b.model.scrollBottom()) {
        return "scroll region";
    }
    if (a.model.originMode() != b.model.originMode()) {
        return "origin mode";
    }
    const auto &ma = a.parser.modes(), &mb = b.parser.modes();
    if (ma.applicationCursor != mb.applicationCursor ||
        ma.applicationKeypad != mb.applicationKeypad ||
        ma.bracketedPaste != mb.bracketedPaste ||
        ma.focusReporting != mb.focusReporting ||
        ma.mouseTracking != mb.mouseTracking ||
        ma.mouseEncoding != mb.mouseEncoding) {
        return "modes";
    }
    if (history) {
        if (a.model.historySize() != b.model.historySize()) {
            return "history size " + std::to_string(a.model.historySize()) +
                   " vs " + std::to_string(b.model.historySize());
        }
        for (std::size_t i = 0; i < a.model.historySize(); ++i) {
            if (a.model.historyRowWrapped(i) != b.model.historyRowWrapped(i)) {
                return "wrap of saved row " + std::to_string(i);
            }
            for (std::size_t x = 0; x < a.model.columns(); ++x) {
                if (!sameCell(a.model.historyCell(x, i),
                              b.model.historyCell(x, i))) {
                    return "saved cell " + std::to_string(x) + "," +
                           std::to_string(i);
                }
            }
        }
    }
    return {};
}

// The program's output in `a`; its snapshot written into the fresh `b`.
std::string roundTrip(const std::string &output,
                      Term &a,
                      Term &b,
                      tty::SnapshotOptions options = {}) {
    a.feed(output);
    b.feed(tty::snapshot(a.model, a.parser.modes(), options));
    return difference(a, b, options.scrollback);
}
} // namespace

TEST_SUIT_BEGIN(TtySnapshot)

TEST_CASE("A shell session with more lines than the screen: scrollback and "
          "screen arrive the same") {
    Term a, b;
    std::string out;
    for (int i = 1; i <= 40; ++i) {
        out += "line " + std::to_string(i) + " of the output\r\n";
    }
    out += "$ prompt";
    EXPECT_EQ(roundTrip(out, a, b), std::string());
    EXPECT(a.model.historySize() > 30);
}

TEST_CASE("Colours and attributes of every kind, and a coloured blank at the "
          "end of a line") {
    Term a, b;
    const std::string out =
        "\x1b[1mbold\x1b[0m \x1b[3mital\x1b[23m \x1b[4mul\x1b[24m "
        "\x1b[7minv\x1b[27m \x1b[9mstrike\x1b[0m\r\n"
        "\x1b[31mred\x1b[32m green\x1b[91m bright\x1b[0m "
        "\x1b[38;5;200mindexed\x1b[0m\r\n"
        "\x1b[38;2;10;200;30mtruecolor\x1b[48;2;1;2;3m on bg\x1b[0m\r\n"
        "\x1b[44m   blue background blank\x1b[K\x1b[0m\r\n"
        "\x1b[2mfaint\x1b[0m \x1b[5mblink\x1b[0m \x1b[8mhidden\x1b[0m\r\n";
    EXPECT_EQ(roundTrip(out, a, b), std::string());
}

TEST_CASE("The cursor: position, hidden, shape") {
    {
        Term a, b;
        EXPECT_EQ(roundTrip("one\r\ntwo\r\n\x1b[3;5Hx\x1b[2;2H", a, b),
                  std::string());
    }
    {
        Term a, b;
        EXPECT_EQ(roundTrip("text\x1b[?25l", a, b), std::string());
        EXPECT(!b.model.cursor().visible);
    }
    {
        Term a, b;
        EXPECT_EQ(roundTrip("text\x1b[6 q", a, b),
                  std::string()); // Steady bar.
    }
}

TEST_CASE("A long line that wrapped keeps its wrap, in the screen and in the "
          "saved rows") {
    Term a, b;
    std::string out;
    for (int i = 0; i < 3; ++i) {
        out += std::string(50, char('a' + i)) +
               "\r\n"; // 50 chars in 20 columns: three rows each.
    }
    out += std::string(45, 'z'); // Still on the screen, wrapped.
    EXPECT_EQ(roundTrip(out, a, b), std::string());
    bool anyWrapped = false;
    for (std::size_t i = 0; i < b.model.historySize(); ++i) {
        anyWrapped = anyWrapped || b.model.historyRowWrapped(i);
    }
    EXPECT(anyWrapped);
}

TEST_CASE("Scroll region, origin mode and a cursor inside the region") {
    Term a, b;
    EXPECT_EQ(
        roundTrip("header\x1b[2;5r\x1b[?6h\x1b[2;3Hin region\x1b[?6l", a, b),
        std::string());
    Term c, d;
    EXPECT_EQ(roundTrip("top\x1b[2;5r\x1b[?6hx\x1b[3;1Hy", c, d),
              std::string());
}

TEST_CASE("The input modes the program asked for") {
    Term a, b;
    EXPECT_EQ(
        roundTrip("\x1b[?1h\x1b=\x1b[?2004h\x1b[?1004h\x1b[?1002h\x1b[?1006h$ ",
                  a,
                  b),
        std::string());
    EXPECT(b.parser.modes().applicationCursor &&
           b.parser.modes().bracketedPaste && b.parser.modes().focusReporting);
    EXPECT(b.parser.modes().mouseTracking ==
           TerminalModes::MouseTracking::Drag);
    EXPECT(b.parser.modes().mouseEncoding == TerminalModes::MouseEncoding::Sgr);
    for (const char *track : {"\x1b[?9h", "\x1b[?1000h", "\x1b[?1003h"}) {
        for (const char *encode : {"", "\x1b[?1005h", "\x1b[?1015h"}) {
            Term c, d;
            EXPECT_EQ(roundTrip(std::string("hi") + track + encode, c, d),
                      std::string());
        }
    }
}

TEST_CASE("A program in the alternate screen: the host enters it and shows the "
          "screen") {
    Term a, b;
    std::string out = "shell line 1\r\nshell line 2\r\n$ vim\r\n";
    out += "\x1b[?1049h\x1b[2J\x1b[H\x1b[1;1H\x1b[7m file.txt "
           "\x1b[0m\x1b[3;1Htext in the editor\x1b[6;1H-- INSERT --\x1b[3;5H";
    a.feed(out);
    EXPECT(a.model.alternateScreen());
    b.feed(tty::snapshot(a.model, a.parser.modes()));
    EXPECT(b.model.alternateScreen());
    EXPECT_EQ(difference(a, b, false), std::string());
    // When the program leaves, the host is back in the normal screen.
    b.feed(tty::hostRestore(true));
    EXPECT(!b.model.alternateScreen());
}

TEST_CASE(
    "Whatever the host showed before, the visible rows are the model's rows") {
    Term a, b;
    std::string junk;
    for (int i = 0; i < 15; ++i) {
        junk += "junk " + std::to_string(i) + "\r\n";
    }
    b.feed(junk + "\x1b[3;7H"); // Content, cursor in the middle.
    a.feed("alpha\r\nbeta\r\n\x1b[4;3Hgamma\x1b[2;1H");
    const auto savedBefore = b.model.historySize();
    b.feed(tty::snapshot(a.model, a.parser.modes()));
    EXPECT_EQ(difference(a, b, false), std::string());
    EXPECT(b.model.historySize() >=
           savedBefore); // What was there is in the scrollback, not lost.
}

TEST_CASE("Wide characters are written as the model holds them") {
    Term a, b;
    EXPECT_EQ(roundTrip("\xe4\xb8\x96\xe7\x95\x8c ok\r\nabc \xf0\x9f\x94\xa5 x",
                        a,
                        b),
              std::string());
}

TEST_CASE("Without scrollback only the screen is written, and a limit keeps "
          "the newest rows") {
    Term a, b;
    std::string out;
    for (int i = 1; i <= 30; ++i) {
        out += "L" + std::to_string(i) + "\r\n";
    }
    tty::SnapshotOptions none;
    none.scrollback = false;
    EXPECT_EQ(roundTrip(out, a, b, none), std::string());
    EXPECT_EQ(b.model.historySize(), std::size_t(0));
    Term c, d;
    c.feed(out);
    tty::SnapshotOptions few;
    few.maxScrollbackRows = 5;
    d.feed(tty::snapshot(c.model, c.parser.modes(), few));
    EXPECT_EQ(d.model.historySize(), std::size_t(5));
    EXPECT_EQ(difference(c, d, false), std::string());
    // The newest five saved rows: the row right above the screen is the same.
    EXPECT(sameCell(c.model.historyCell(0, c.model.historySize() - 1),
                    d.model.historyCell(0, 4)));
}

TEST_CASE("An empty terminal and a one-line terminal") {
    Term a, b;
    EXPECT_EQ(roundTrip("", a, b), std::string());
    Term c(5, 1), d(5, 1);
    EXPECT_EQ(roundTrip("a\r\nb\r\nc", c, d), std::string());
}

TEST_CASE(
    "The restore sequence turns the host back, whatever the program had on") {
    Term a, b;
    a.feed("\x1b[?1h\x1b=\x1b[?2004h\x1b[?1004h\x1b[?1003h\x1b[?1006h\x1b[2;"
           "4r\x1b[?25l\x1b[5 q\x1b[1;31m");
    b.feed(tty::snapshot(a.model, a.parser.modes()));
    EXPECT(b.parser.modes().mouseTracking != TerminalModes::MouseTracking::Off);
    b.feed(tty::hostRestore(false));
    const auto &m = b.parser.modes();
    EXPECT(!m.applicationCursor && !m.applicationKeypad && !m.bracketedPaste &&
           !m.focusReporting);
    EXPECT(m.mouseTracking == TerminalModes::MouseTracking::Off);
    EXPECT(m.mouseEncoding == TerminalModes::MouseEncoding::X10);
    EXPECT(b.model.cursor().visible);
    EXPECT_EQ(b.model.scrollTop(), 0);
    EXPECT_EQ(b.model.scrollBottom(), int(b.model.rows()) - 1);
    EXPECT(!b.model.alternateScreen());
    // The next output starts on a fresh line, with the pen reset.
    EXPECT_EQ(b.model.cursor().column, 0);
    b.feed("after");
    EXPECT(b.model.cell(0, std::size_t(b.model.cursor().row)).style() ==
           CellAttribute::None);
}

TEST_CASE("Primary restore appends below the bottom row without clearing content, even after resize") {
    for (const auto rows : {1, 3, 24}) {
        Term host(40, 30);
        host.model.resize(20, std::size_t(rows));
        host.feed("\x1b[" + std::to_string(rows) + ";1Hbottom-kept\x1b[1;1H");
        host.feed(tty::hostRestore(false));
        EXPECT_EQ(host.model.cursor().row, rows - 1);
        EXPECT_EQ(host.model.cursor().column, 0);
        if (rows > 1) {
            EXPECT_EQ(host.model.cell(0, std::size_t(rows - 2)).codePoint, U'b');
        } else {
            EXPECT(host.model.historySize() > 0);
            EXPECT_EQ(host.model.historyCell(0, host.model.historySize() - 1).codePoint, U'b');
        }
    }
    const auto alt = tty::hostRestore(true);
    EXPECT(alt.find("\x1b[65535;1H") == std::string::npos);
    EXPECT(alt.find("\x1b[?1049l\r\n") != std::string::npos);
}

TEST_CASE(
    "Random output of every kind arrives the same (a seeded property test)") {
    std::uint64_t state = 12345;
    const auto next = [&](unsigned bound) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return unsigned((state >> 33) % bound);
    };
    const char *pieces[] = {"\x1b[1m",   "\x1b[0m",       "\x1b[31m",
                            "\x1b[44m",  "\x1b[38;5;99m", "\x1b[38;2;9;99;199m",
                            "\x1b[7m",   "\x1b[3m",       "\x1b[4m",
                            "\x1b[2J",   "\x1b[K",        "\x1b[1K",
                            "\x1b[2K",   "\x1b[H",        "\x1b[L",
                            "\x1b[M",    "\x1b[P",        "\x1b[@",
                            "\x1b[S",    "\x1b[T",        "\x1bM",
                            "\x1b[?25l", "\x1b[?25h",     "\x1b[3 q",
                            "\r\n",      "\r\n",          "\r\n",
                            "\t",        "\x08"};
    for (int round = 0; round < 150; ++round) {
        Term a(unsigned(5 + next(30)), unsigned(2 + next(10)), 60),
            b(a.model.columns(), a.model.rows(), 60);
        std::string out = round % 5 == 0
                              ? "some text first\r\n\x1b[?1049h"
                              : ""; // A fifth of them in the alternate screen.
        for (int i = 0, n = 20 + int(next(200)); i < n; ++i) {
            switch (next(6)) {
            case 0:
                out += pieces[next(sizeof pieces / sizeof *pieces)];
                break;
            case 1:
                out += "\x1b[" + std::to_string(1 + next(12)) + ";" +
                       std::to_string(1 + next(40)) + "H";
                break;
            case 2:
                if (next(8) == 0) {
                    const auto top = 1 + next(4);
                    out += "\x1b[" + std::to_string(top) + ";" +
                           std::to_string(top + 1 + next(5)) + "r";
                }
                break;
            default:
                for (unsigned k = 0, m = 1 + next(40); k < m; ++k) {
                    out += char('a' + next(26));
                }
                break;
            }
        }
        const auto diff = roundTrip(out, a, b);
        if (!diff.empty()) {
            (void)std::fprintf(stderr,
                         "round %d: %s (%zux%zu)\n",
                         round,
                         diff.c_str(),
                         a.model.columns(),
                         a.model.rows());
        }
        EXPECT_EQ(diff, std::string());
    }
}

TEST_SUIT_END
