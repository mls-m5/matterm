#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "tty/screen.h"
#include "vtparser.h"

using namespace tty;

namespace {
struct Host {
    TerminalModel model;
    VtParser parser;
    Host(int columns, int rows)
        : model(std::size_t(columns), std::size_t(rows))
        , parser(model) {}
    void feed(const std::string &bytes) {
        parser.feed(bytes);
    }
};
bool has(const std::string &text, const char *part) {
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_SUIT_BEGIN(TtyScreen)

TEST_CASE("enter and leave switch the alternate screen and hide the cursor") {
    Screen screen(10, 3);
    Host host(10, 3);
    const auto in = screen.enter();
    host.feed(in);
    EXPECT(host.model.alternateScreen());
    EXPECT(!host.model.cursor().visible);
    EXPECT(screen.entered());
    EXPECT(screen.enter().empty()); // Once is enough.
    const auto out = screen.leave();
    host.feed(out);
    EXPECT(!host.model.alternateScreen());
    EXPECT(host.model.cursor().visible);
    EXPECT(!screen.entered());
    EXPECT(screen.leave().empty());
}

TEST_CASE("A terminal without an alternate screen is cleared and left below") {
    Caps caps;
    caps.alternateScreen = false;
    Screen screen(10, 3, caps);
    EXPECT(!has(screen.enter(), "?1049"));
    const auto out = screen.leave();
    EXPECT(!has(out, "?1049"));
    EXPECT(has(out, "\x1b[3;1H")); // Below the drawing.
}

TEST_CASE("Mouse, paste and focus reports are asked for and given back") {
    ScreenOptions options;
    options.mouse = options.bracketedPaste = options.focusEvents = true;
    Screen screen(10, 3, {}, options);
    const auto in = screen.enter();
    EXPECT(has(in, "?1002h") && has(in, "?1006h"));
    EXPECT(has(in, "?2004h"));
    EXPECT(has(in, "?1004h"));
    const auto out = screen.leave();
    EXPECT(has(out, "?1002l") && has(out, "?1006l"));
    EXPECT(has(out, "?2004l"));
    EXPECT(has(out, "?1004l"));
    Screen plain(10, 3);
    EXPECT(!has(plain.enter(), "?1002h"));
    EXPECT(!has(plain.enter(), "?2004h"));
}

TEST_CASE("present draws the canvas and puts the cursor where it belongs") {
    Screen screen(10, 3);
    Host host(10, 3);
    host.feed(screen.enter());
    screen.canvas().text(2, 1, "hello", {255, 255, 255});
    screen.setCursor({true, 7, 1});
    host.feed(screen.present());
    EXPECT(host.model.cell(2, 1).codePoint == U'h');
    EXPECT(host.model.cell(6, 1).codePoint == U'o');
    EXPECT_EQ(host.model.cursor().column, 7);
    EXPECT_EQ(host.model.cursor().row, 1);
    EXPECT(host.model.cursor().visible);
}

TEST_CASE("An unchanged screen and cursor present nothing") {
    Screen screen(10, 3);
    screen.enter();
    screen.canvas().text(0, 0, "x", {1, 1, 1});
    screen.setCursor({true, 1, 0});
    EXPECT(!screen.present().empty());
    EXPECT(screen.present().empty());
    screen.setCursor({true, 2, 0}); // Only the cursor moved.
    const auto out = screen.present();
    EXPECT(!out.empty() && out.size() < 20);
    screen.setCursor({false, 2, 0});
    EXPECT(has(screen.present(), "?25l"));
    EXPECT(screen.present().empty());
}

TEST_CASE("The cursor is hidden while cells are written, then restored") {
    Screen screen(10, 3);
    screen.enter();
    screen.setCursor({true, 0, 0});
    screen.present();
    screen.canvas().text(5, 2, "z", {1, 1, 1});
    const auto out = screen.present();
    const auto hide = out.find("?25l");
    const auto cell = out.find('z');
    const auto show = out.rfind("?25h");
    EXPECT(hide != std::string::npos && cell != std::string::npos &&
           show != std::string::npos);
    EXPECT(hide < cell && cell < show);
    EXPECT(has(out, "?2026h") && has(out, "?2026l")); // Synchronized update.
}

TEST_CASE("Cursor shapes use DECSCUSR and the default is restored on leave") {
    Screen screen(10, 3);
    screen.enter();
    Cursor cursor{true, 0, 0};
    cursor.shape = Cursor::Shape::Bar;
    cursor.blink = false;
    screen.setCursor(cursor);
    EXPECT(has(screen.present(), "\x1b[6 q"));
    cursor.shape = Cursor::Shape::Block;
    cursor.blink = true;
    screen.setCursor(cursor);
    EXPECT(has(screen.present(), "\x1b[1 q"));
    cursor.shape = Cursor::Shape::Underline;
    screen.setCursor(cursor);
    EXPECT(has(screen.present(), "\x1b[3 q"));
    EXPECT(has(screen.leave(), "\x1b[0 q"));
}

TEST_CASE("resize clears the host screen and redraws every cell") {
    Screen screen(10, 3);
    Host host(10, 3);
    host.feed(screen.enter());
    screen.canvas().text(0, 0, "abcdefghij", {1, 1, 1});
    host.feed(screen.present());
    screen.resize(6, 2);
    EXPECT_EQ(screen.columns(), 6);
    EXPECT_EQ(screen.rows(), 2);
    screen.canvas().text(0, 0, "xyz", {1, 1, 1});
    const auto out = screen.present();
    EXPECT(has(out, "\x1b[2J"));
    Host small(6, 2);
    small.feed(out);
    EXPECT(small.model.cell(0, 0).codePoint == U'x');
    EXPECT(small.model.cell(5, 1).codePoint == U' ');
}

TEST_CASE("redraw forces the next present to repaint, e.g. after a stop") {
    Screen screen(5, 1);
    screen.enter();
    screen.canvas().text(0, 0, "hi", {1, 1, 1});
    screen.present();
    EXPECT(screen.present().empty());
    screen.redraw();
    EXPECT(has(screen.present(), "hi"));
}

TEST_CASE(
    "Without a UTF-8 terminal non-ASCII characters become question marks") {
    Caps caps;
    caps.unicode = false;
    Screen screen(4, 1, caps);
    screen.enter();
    screen.canvas().put(0, 0, U'\u00e5', {1, 1, 1}, {0, 0, 0});
    screen.canvas().put(1, 0, U'\u4e16', {1, 1, 1}, {0, 0, 0});
    screen.canvas().put(2, 0, U'a', {1, 1, 1}, {0, 0, 0});
    const auto out = screen.present();
    EXPECT(has(out, "??a"));
    EXPECT(!has(out, "\xc3"));
}

TEST_CASE("Wide characters are handled in a Screen") {
    Screen screen(6, 1);
    screen.enter();
    screen.canvas().put(0, 0, U'世', {1, 1, 1}, {0, 0, 0});
    screen.canvas().put(1, 0, U'x', {1, 1, 1}, {0, 0, 0});
    EXPECT(!has(screen.present(), "x"));
}

TEST_SUIT_END
