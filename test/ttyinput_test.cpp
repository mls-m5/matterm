#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "ttyinput.h"
#include "vtparser.h"

#include <string>
#include <vector>

namespace {
using namespace frontend::tty;
using Kind = InputEvent::Kind;
using Key = InputEncoder::Key;

std::vector<InputEvent> decode(const std::string &bytes) {
    HostInputDecoder decoder;
    auto events = decoder.feed(bytes);
    for (auto &e : decoder.flush()) {
        events.push_back(std::move(e));
    }
    return events;
}

// What the program gets for `bytes` typed in the host terminal, under the modes
// it set up.
std::string forProgram(const std::string &bytes,
                       const std::string &programOutput = "") {
    TerminalModel model(20, 5);
    VtParser parser(model);
    parser.feed(programOutput);
    std::string sent;
    for (const auto &e : decode(bytes)) {
        sent += encodeForProgram(e, parser.modes());
    }
    return sent;
}
} // namespace

TEST_SUIT_BEGIN(TtyInput)

TEST_CASE("Plain text passes through as bytes, Enter is a key") {
    const auto events = decode("ls -l\r");
    EXPECT_EQ(events.size(), std::size_t(2));
    EXPECT(events[0].kind == Kind::Bytes && events[0].text == "ls -l");
    EXPECT(events[1].kind == Kind::Key && events[1].key == Key::Enter);
    EXPECT_EQ(forProgram("åäö\r"), std::string("åäö\r"));
}

TEST_CASE("Arrow keys follow the application cursor mode of the program") {
    EXPECT_EQ(forProgram("\x1b[A"), std::string("\x1b[A"));
    EXPECT_EQ(forProgram("\x1b[A", "\x1b[?1h"), std::string("\x1bOA"));
    EXPECT_EQ(forProgram("\x1bOB"),
              std::string("\x1b[B")); // The host in application mode.
    EXPECT_EQ(forProgram("\x1b[5~\x1b[3~"), std::string("\x1b[5~\x1b[3~"));
    const auto ctrlRight = decode("\x1b[1;5C");
    EXPECT_EQ(ctrlRight.size(), std::size_t(1));
    EXPECT(ctrlRight[0].key == Key::Right && ctrlRight[0].modifiers.control &&
           !ctrlRight[0].modifiers.shift);
}

TEST_CASE("A lone Escape is a key only once the input has gone quiet") {
    HostInputDecoder decoder;
    EXPECT(decoder.feed("\x1b").empty());
    EXPECT(decoder.pending());
    const auto flushed = decoder.flush();
    EXPECT_EQ(flushed.size(), std::size_t(1));
    EXPECT(flushed[0].kind == Kind::Key && flushed[0].key == Key::Escape);
    EXPECT(!decoder.pending());
}

TEST_CASE("A sequence split between two reads is still one key") {
    HostInputDecoder decoder;
    EXPECT(decoder.feed("\x1b[").empty());
    EXPECT(decoder.feed("1;2").empty());
    const auto events = decoder.feed("A");
    EXPECT_EQ(events.size(), std::size_t(1));
    EXPECT(events[0].key == Key::Up && events[0].modifiers.shift);
}

TEST_CASE("Alt and keys we do not decode are sent as they came") {
    EXPECT_EQ(forProgram("\x1b"
                         "b"),
              std::string("\x1b"
                          "b"));
    EXPECT_EQ(forProgram("\x1b[Z"), std::string("\x1b[Z")); // Shift+Tab.
    EXPECT_EQ(forProgram("\x7f"), std::string("\x7f"));
}

TEST_CASE("The leave key is its own event, nothing after it is lost") {
    const auto events = decode("a\x1d"
                               "b");
    EXPECT_EQ(events.size(), std::size_t(3));
    EXPECT(events[0].text == "a");
    EXPECT(events[1].kind == Kind::Leave);
    EXPECT(events[2].text == "b");
    EXPECT_EQ(std::string(kLeaveName), std::string("Ctrl+]"));
}

TEST_CASE(
    "Bracketed paste is unpacked and packed again only if the program asked") {
    EXPECT_EQ(forProgram("\x1b[200~one\ntwo\x1b[201~"),
              std::string("one\rtwo"));
    EXPECT_EQ(forProgram("\x1b[200~one\x1b[201~", "\x1b[?2004h"),
              std::string("\x1b[200~one\x1b[201~"));
    // Escape characters in the text are taken out (InputEncoder::paste).
    EXPECT_EQ(forProgram("\x1b[200~a\x1b[31mb\x1b[201~", "\x1b[?2004h"),
              std::string("\x1b[200~a[31mb\x1b[201~"));
}

TEST_CASE("A paste that arrives in pieces, with the end marker cut in two") {
    HostInputDecoder decoder;
    EXPECT(decoder.feed("\x1b[200~hel").empty());
    EXPECT(decoder.feed("lo\x1b[20").empty());
    const auto events = decoder.feed("1~x");
    EXPECT_EQ(events.size(), std::size_t(2));
    EXPECT(events[0].kind == Kind::Paste && events[0].text == "hello");
    EXPECT(events[1].kind == Kind::Bytes && events[1].text == "x");
}

TEST_CASE("Mouse reports are decoded and encoded for the program's encoding") {
    // The host sends SGR (button 0 press at column 10, row 3); the program
    // wants X10.
    EXPECT_EQ(
        forProgram("\x1b[<0;10;3M", "\x1b[?1000h"),
        std::string(
            "\x1b[M *#")); // 32+0, 32+10, 32+3 (column and row count from 1).
    // Nothing reaches a program that did not ask for the mouse.
    EXPECT_EQ(forProgram("\x1b[<0;10;3M"), std::string());
    // SGR to SGR, release, wheel.
    EXPECT_EQ(
        forProgram("\x1b[<0;10;3M\x1b[<0;10;3m", "\x1b[?1000h\x1b[?1006h"),
        std::string("\x1b[<0;10;3M\x1b[<0;10;3m"));
    EXPECT_EQ(forProgram("\x1b[<64;1;1M", "\x1b[?1000h\x1b[?1006h"),
              std::string("\x1b[<64;1;1M"));
    const auto drag = decode("\x1b[<32;5;6M");
    EXPECT_EQ(drag.size(), std::size_t(1));
    EXPECT(drag[0].mouse.type == InputEncoder::MouseEvent::Type::Motion);
    EXPECT_EQ(drag[0].mouse.column, 4);
    EXPECT_EQ(drag[0].mouse.row, 5);
    // A report that is nonsense is dropped, not typed into the program.
    EXPECT_EQ(forProgram("\x1b[<0;0;0M", "\x1b[?1000h"), std::string());
}

TEST_CASE("The host reports the mouse only while the program wants it") {
    TerminalModes modes;
    EXPECT(!hostModesFor(modes, true).mouse);
    modes.mouseTracking = TerminalModes::MouseTracking::Normal;
    EXPECT(hostModesFor(modes, true).mouse &&
           !hostModesFor(modes, true).mouseMotion);
    EXPECT(!hostModesFor(modes, false)
                .mouse); // Not live: the list or a notice, no mouse.
    modes.mouseTracking = TerminalModes::MouseTracking::Any;
    EXPECT(hostModesFor(modes, true).mouseMotion &&
           hostModesFor(modes, true).mouseAnyMotion);
    modes.mouseTracking = TerminalModes::MouseTracking::Drag;
    EXPECT(hostModesFor(modes, true).mouseMotion &&
           !hostModesFor(modes, true).mouseAnyMotion);
}

TEST_SUIT_END
