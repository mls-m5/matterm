#include "keys.h"
#include "mls-unit-test/unittest.h"

using fakeos::Key;
using fakeos::KeyDecoder;

TEST_SUIT_BEGIN(FakeosKeys)

TEST_CASE("Text, UTF-8 and control keys") {
    KeyDecoder d;
    auto keys = d.feed("a\xc3\xa5\xe2\x82\xac\r\x7f\t\x03");
    EXPECT_EQ(keys.size(), std::size_t(7));
    EXPECT(keys[0].isText("a"));
    EXPECT(keys[1].isText("\xc3\xa5"));
    EXPECT(keys[2].isText("\xe2\x82\xac"));
    EXPECT(keys[3].is(Key::Type::Enter));
    EXPECT(keys[4].is(Key::Type::Backspace));
    EXPECT(keys[5].is(Key::Type::Tab));
    EXPECT(keys[6].is(Key::Type::Ctrl) && keys[6].text == "c");
}

TEST_CASE("UTF-8 split across chunks") {
    KeyDecoder d;
    EXPECT(d.feed("\xe2\x82").empty());
    const auto keys = d.feed("\xac");
    EXPECT_EQ(keys.size(), std::size_t(1));
    EXPECT(keys[0].isText("\xe2\x82\xac"));
}

TEST_CASE("Cursor and editing keys in CSI and SS3 form") {
    KeyDecoder d;
    const auto keys = d.feed(
        "\x1b[A\x1b[B\x1b[C\x1b[D\x1bOH\x1b[F\x1b[3~\x1b[5~\x1b[6~\x1b[1;5C");
    EXPECT_EQ(keys.size(), std::size_t(10));
    EXPECT(keys[0].is(Key::Type::Up));
    EXPECT(keys[1].is(Key::Type::Down));
    EXPECT(keys[2].is(Key::Type::Right));
    EXPECT(keys[3].is(Key::Type::Left));
    EXPECT(keys[4].is(Key::Type::Home));
    EXPECT(keys[5].is(Key::Type::End));
    EXPECT(keys[6].is(Key::Type::Delete));
    EXPECT(keys[7].is(Key::Type::PageUp));
    EXPECT(keys[8].is(Key::Type::PageDown));
    EXPECT(keys[9].is(Key::Type::Right)); // With a modifier parameter.
}

TEST_CASE("A lone ESC is Escape; ESC plus a key is Alt") {
    KeyDecoder d;
    auto keys = d.feed("\x1b");
    EXPECT_EQ(keys.size(), std::size_t(1));
    EXPECT(keys[0].is(Key::Type::Escape));
    keys = d.feed("\x1b"
                  "f");
    EXPECT_EQ(keys.size(), std::size_t(1));
    EXPECT(keys[0].isText("f") && keys[0].alt);
}

TEST_CASE("SGR mouse reports") {
    KeyDecoder d;
    auto keys = d.feed("\x1b[<0;10;5M\x1b[<0;10;5m\x1b[<64;3;4M\x1b[<35;7;8M");
    EXPECT_EQ(keys.size(), std::size_t(4));
    EXPECT(keys[0].is(Key::Type::Mouse) && keys[0].press &&
           keys[0].button == 0);
    EXPECT_EQ(keys[0].column, 9);
    EXPECT_EQ(keys[0].row, 4);
    EXPECT(!keys[1].press);
    EXPECT_EQ(keys[2].button, 64);
    EXPECT(keys[3].motion);
}

TEST_CASE("Replies and bracketed paste do not become keys") {
    KeyDecoder d;
    auto keys =
        d.feed("\x1b[12;40R\x1b]11;rgb:0/0/0\x1b\\\x1b[200~hi\r\nx\x1b[201~\r");
    // "hi", newline, "x" as text, then a real Enter.
    EXPECT_EQ(keys.size(), std::size_t(5));
    EXPECT(keys[0].isText("h"));
    EXPECT(keys[2].isText("\n"));
    EXPECT(keys[4].is(Key::Type::Enter));
}

TEST_SUIT_END
