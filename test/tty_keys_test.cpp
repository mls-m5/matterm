#include "mls-unit-test/unittest.h"
#include "tty/keys.h"

using tty::Key;
using tty::KeyDecoder;

TEST_SUIT_BEGIN(TtyKeys)

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

TEST_CASE("Word keys: Alt+Backspace, Alt+D, Ctrl+Delete, Ctrl and Alt with the "
          "arrows") {
    KeyDecoder d;
    Key k;
    auto one = [&](const char *bytes) {
        const auto keys = d.feed(bytes);
        EXPECT_EQ(keys.size(), std::size_t(1));
        k = keys.empty() ? Key{} : keys[0];
    };
    one("\x1b\x7f"); // Alt+Backspace
    EXPECT(k.is(Key::Type::Backspace) && k.alt && !k.ctrl);
    one("\x1b"
        "d"); // Alt+D
    EXPECT(k.isText("d") && k.alt);
    one("\x1b[3;5~"); // Ctrl+Delete
    EXPECT(k.is(Key::Type::Delete) && k.ctrl && !k.alt);
    one("\x1b[3;3~"); // Alt+Delete
    EXPECT(k.is(Key::Type::Delete) && k.alt && !k.ctrl);
    one("\x1b[1;5D"); // Ctrl+Left
    EXPECT(k.is(Key::Type::Left) && k.ctrl);
    one("\x1b[1;3C"); // Alt+Right
    EXPECT(k.is(Key::Type::Right) && k.alt);
    one("\x1b[1;7D"); // Ctrl+Alt+Left
    EXPECT(k.is(Key::Type::Left) && k.ctrl && k.alt);
    one("\x1b[3~"); // Plain Delete: no modifiers.
    EXPECT(k.is(Key::Type::Delete) && !k.ctrl && !k.alt);
    one("\x1b[D");
    EXPECT(k.is(Key::Type::Left) && !k.ctrl && !k.alt);
    one("\x17"); // Ctrl+Backspace arrives as Ctrl-W.
    EXPECT(k.is(Key::Type::Ctrl) && k.text == "w");
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

TEST_CASE("Function keys, Insert and Shift+Tab") {
    KeyDecoder d;
    auto one = [&](const char *bytes) {
        const auto keys = d.feed(bytes);
        return keys.size() == 1
                   ? keys[0]
                   : Key{Key::Type::Escape}; // Not what it should be.
    };
    EXPECT(one("\x1bOP").is(Key::Type::Function) && one("\x1bOP").number == 1);
    EXPECT_EQ(one("\x1bOS").number, 4);
    EXPECT_EQ(one("\x1b[11~").number, 1);
    EXPECT_EQ(one("\x1b[15~").number, 5);
    EXPECT_EQ(one("\x1b[17~").number, 6);
    EXPECT_EQ(one("\x1b[21~").number, 10);
    EXPECT_EQ(one("\x1b[23~").number, 11);
    EXPECT_EQ(one("\x1b[24~").number, 12);
    const auto f5 = one("\x1b[15;5~"); // Ctrl+F5
    EXPECT(f5.number == 5 && f5.ctrl && !f5.shift);
    const auto f2 = one("\x1b[1;2Q"); // Shift+F2
    EXPECT(f2.is(Key::Type::Function) && f2.number == 2 && f2.shift);
    EXPECT(one("\x1b[2~").is(Key::Type::Insert));
    const auto back = one("\x1b[Z");
    EXPECT(back.is(Key::Type::Tab) && back.shift);
    EXPECT(!one("\t").shift);
}

TEST_CASE("Shift on arrows, and a cursor position report is no key") {
    KeyDecoder d;
    const auto keys = d.feed("\x1b[1;2A\x1b[1;6C");
    EXPECT_EQ(keys.size(), std::size_t(2));
    EXPECT(keys[0].is(Key::Type::Up) && keys[0].shift && !keys[0].ctrl);
    EXPECT(keys[1].is(Key::Type::Right) && keys[1].shift && keys[1].ctrl);
    EXPECT(d.feed("\x1b[24;80R").empty()); // Not F3.
    EXPECT(d.feed("\x1b[?1;2c").empty());
}

TEST_CASE("Every key keeps the exact bytes it came from") {
    KeyDecoder d;
    const std::string input = "a\xc3\xa5\r\x7f\x03\x1b[A\x1b[1;5D\x1bOP\x1b[15~"
                              "\x1bx\x1b[<0;5;6M\t\x1b[Z";
    const auto keys = d.feed(input);
    std::string joined;
    for (const auto &key : keys) {
        EXPECT(!key.raw.empty());
        joined += key.raw;
    }
    EXPECT(joined == input);
    EXPECT(keys[1].raw == "\xc3\xa5");
    EXPECT(keys[5].raw == "\x1b[A");
    EXPECT(keys[9].raw == "\x1bx"); // Alt+x
    EXPECT(keys[9].alt);
}

TEST_CASE("Raw bytes survive being split across chunks, and replies leave none "
          "behind") {
    KeyDecoder d;
    std::string joined;
    for (const char *part : {"\x1b", "[1;", "5D", "\xe2\x82", "\xac"}) {
        for (const auto &key : d.feed(part)) {
            joined += key.raw;
        }
    }
    EXPECT(joined == "\x1b[1;5D\xe2\x82\xac");
    EXPECT(d.feed("\x1b[24;80R").empty());
    const auto after = d.feed("z");
    EXPECT_EQ(after.size(), std::size_t(1));
    EXPECT(after[0].raw == "z");
}

TEST_SUIT_END
