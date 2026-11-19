#include "inputencoder.h"
#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

TEST_SUIT_BEGIN(InputEncoder)

TEST_CASE("Application cursor and bracketed paste modes are negotiated by VT") {
    TerminalModel model(10, 2);
    VtParser parser(model);
    using Key = InputEncoder::Key;
    EXPECT_EQ(InputEncoder::key(Key::Up, {}, parser.modes()),
              std::string("\x1b[A"));
    parser.feed("\x1b[?1h\x1b[?2004h");
    EXPECT_EQ(InputEncoder::key(Key::Up, {}, parser.modes()),
              std::string("\x1bOA"));
    EXPECT_EQ(InputEncoder::paste("hello", parser.modes()),
              std::string("\x1b[200~hello\x1b[201~"));
    parser.feed("\x1b[?1l\x1b[?2004l");
    EXPECT_EQ(InputEncoder::key(Key::Up, {}, parser.modes()),
              std::string("\x1b[A"));
    EXPECT_EQ(InputEncoder::paste("hello", parser.modes()),
              std::string("hello"));
}

TEST_CASE("Mouse reports follow the negotiated tracking and encoding") {
    using Event = InputEncoder::MouseEvent;
    TerminalModel model(10, 2);
    VtParser parser(model);
    Event press;
    press.column = 4;
    press.row = 2;
    EXPECT_EQ(InputEncoder::mouse(press, parser.modes()), std::string());

    parser.feed("\x1b[?1000h");
    EXPECT_EQ(InputEncoder::mouse(press, parser.modes()),
              std::string("\x1b[M %#")); // button 0, column 5, row 3.
    Event release = press;
    release.type = Event::Type::Release;
    EXPECT_EQ(InputEncoder::mouse(release, parser.modes()),
              std::string("\x1b[M#%#"));
    Event motion = press;
    motion.type = Event::Type::Motion;
    EXPECT_EQ(InputEncoder::mouse(motion, parser.modes()), std::string());

    parser.feed("\x1b[?1002h"); // Drag replaces normal tracking.
    EXPECT_EQ(InputEncoder::mouse(motion, parser.modes()),
              std::string("\x1b[M@%#"));
    motion.button = Event::Button::None;
    EXPECT_EQ(InputEncoder::mouse(motion, parser.modes()), std::string());
    parser.feed("\x1b[?1003h");
    EXPECT_EQ(InputEncoder::mouse(motion, parser.modes()),
              std::string("\x1b[MC%#"));

    parser.feed("\x1b[?1006h");
    press.modifiers.control = true;
    EXPECT_EQ(InputEncoder::mouse(press, parser.modes()),
              std::string("\x1b[<16;5;3M"));
    release.button = Event::Button::Right;
    EXPECT_EQ(InputEncoder::mouse(release, parser.modes()),
              std::string("\x1b[<2;5;3m"));
    Event wheel = press;
    wheel.modifiers = {};
    wheel.button = Event::Button::WheelDown;
    EXPECT_EQ(InputEncoder::mouse(wheel, parser.modes()),
              std::string("\x1b[<65;5;3M"));
    wheel.type = Event::Type::Release;
    EXPECT_EQ(InputEncoder::mouse(wheel, parser.modes()), std::string());

    parser.feed("\x1b[?1006l\x1b[?1015h");
    EXPECT_EQ(InputEncoder::mouse(press, parser.modes()),
              std::string("\x1b[48;5;3M"));
    parser.feed("\x1b[?1015l");
    Event far = press;
    far.column = 300;
    EXPECT_EQ(InputEncoder::mouse(far, parser.modes()), std::string());
    parser.feed("\x1b[?1005h");
    EXPECT_EQ(InputEncoder::mouse(far, parser.modes()).size(), std::size_t(7));

    parser.feed("\x1b[?1003l\x1b[?1005l");
    EXPECT_EQ(InputEncoder::mouse(press, parser.modes()), std::string());
}

TEST_CASE("Mouse button 9 reports presses without modifiers") {
    TerminalModel model(10, 2);
    VtParser parser(model);
    parser.feed("\x1b[?9h");
    InputEncoder::MouseEvent event;
    event.modifiers.shift = true;
    EXPECT_EQ(InputEncoder::mouse(event, parser.modes()),
              std::string("\x1b[M !!"));
    event.type = InputEncoder::MouseEvent::Type::Release;
    EXPECT_EQ(InputEncoder::mouse(event, parser.modes()), std::string());
}

TEST_CASE("Focus reports and application keypad are negotiated by VT") {
    TerminalModel model(10, 2);
    VtParser parser(model);
    EXPECT_EQ(InputEncoder::focus(true, parser.modes()), std::string());
    EXPECT_EQ(InputEncoder::keypad('5', {}, parser.modes()), std::string("5"));
    EXPECT_EQ(InputEncoder::keypad('\r', {}, parser.modes()),
              std::string("\r"));
    parser.feed("\x1b[?1004h\x1b=");
    EXPECT_EQ(InputEncoder::focus(true, parser.modes()), std::string("\x1b[I"));
    EXPECT_EQ(InputEncoder::focus(false, parser.modes()),
              std::string("\x1b[O"));
    EXPECT_EQ(InputEncoder::keypad('5', {}, parser.modes()),
              std::string("\x1bOu"));
    EXPECT_EQ(InputEncoder::keypad('\r', {}, parser.modes()),
              std::string("\x1bOM"));
    EXPECT_EQ(InputEncoder::keypad('+', {true, false, false}, parser.modes()),
              std::string("\x1bO2k"));
    parser.feed("\x1b>\x1b[?1004l");
    EXPECT_EQ(InputEncoder::keypad('5', {}, parser.modes()), std::string("5"));
    EXPECT_EQ(InputEncoder::focus(true, parser.modes()), std::string());
}

TEST_CASE("Ctrl Meta modifiers and special keys preserve intended bytes") {
    using Key = InputEncoder::Key;
    EXPECT_EQ(InputEncoder::control('c'), std::string("\x03"));
    EXPECT_EQ(InputEncoder::control(' '), std::string(1, '\0'));
    EXPECT_EQ(InputEncoder::control('['), std::string("\x1b"));
    EXPECT_EQ(InputEncoder::text("å", true), std::string("\x1bå"));
    EXPECT_EQ(InputEncoder::key(Key::Enter, {}, {}), std::string("\r"));
    EXPECT_EQ(InputEncoder::key(Key::Backspace, {}, {}), std::string("\x7f"));
    EXPECT_EQ(InputEncoder::key(Key::Tab, {true, false, false}, {}),
              std::string("\x1b[Z"));
    EXPECT_EQ(InputEncoder::key(Key::Left, {false, false, true}, {}),
              std::string("\x1b[1;5D"));
    EXPECT_EQ(InputEncoder::key(Key::F1, {}, {}), std::string("\x1bOP"));
    EXPECT_EQ(InputEncoder::key(Key::F12, {}, {}), std::string("\x1b[24~"));
}

TEST_CASE("Paste normalizes newlines and strips control-sequence injection") {
    EXPECT_EQ(InputEncoder::paste("a\r\nb\nc\td\x1b[201~\x03", {}),
              std::string("a\rb\rc\td[201~"));
}

TEST_SUIT_END
