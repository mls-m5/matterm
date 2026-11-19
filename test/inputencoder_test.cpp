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
