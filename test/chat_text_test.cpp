#include "chattext.h"
#include "mls-unit-test/unittest.h"

#include <cstdint>
#include <string>

namespace {
using namespace chat;

bool valid(const std::string &s, bool message = true) {
    return (message ? checkMessage(s) : checkDraft(s)).ok();
}

// Everything a sanitized string may contain.
bool clean(const std::string &s) {
    std::u32string cps;
    if (!decodeUtf8Strict(s, cps)) {
        return false;
    }
    for (const char32_t c : cps) {
        if (isControlOrFormat(c) || !(isAllowedChar(c) || c == 0xFFFD)) {
            return false;
        }
    }
    return true;
}

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return std::uint32_t(s >> 33);
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatText)

TEST_CASE("Plain text, Swedish letters and accents are messages") {
    EXPECT(valid("hello everyone"));
    EXPECT(valid("hej! sm\xC3\xA5 \xC3\xA4pplen \xC3\xB6l, caf\xC3\xA9"));
    EXPECT(valid("a"));
    EXPECT(valid(
        "Hello, World: 1+1=2 (really) [yes] {no} <ok> ~#@%&*;'\"?/\\|`^"));
}

TEST_CASE("Controls, escapes and invisible characters are rejected") {
    EXPECT(!valid(std::string("a\0b", 3)));
    EXPECT(!valid("\x1b[31mred"));
    EXPECT(!valid("\x1b]52;c;SGVsbG8=\x07"));
    EXPECT(!valid("tab\there"));
    EXPECT(!valid("line\nbreak"));
    EXPECT(!valid("over\rwrite"));
    EXPECT(!valid("del\x7f"));
    EXPECT(!valid("\xC2\x9B"
                  "31m")); // CSI as a C1 code point
    EXPECT(!valid("\xE2\x80\xAE"
                  "evil"));           // right-to-left override
    EXPECT(!valid("a\xE2\x80\x8Bz")); // zero-width space
    EXPECT(!valid("\xEF\xBB\xBF"
                  "bom")); // byte order mark
    EXPECT(!valid("\xC2\xAD"
                  "soft")); // soft hyphen
    EXPECT(!valid("a\xF3\xA0\x80\x81"
                  "z"));              // a tag character
    EXPECT(!valid("a\xE2\x80\xA8z")); // line separator
    EXPECT(!valid("\x02"
                  "bold\x0F")); // IRC formatting bytes
}

TEST_CASE("Broken UTF-8 is rejected") {
    EXPECT(checkMessage("\xC0\xAF").error == TextError::BadUtf8); // overlong
    EXPECT(checkMessage("\xED\xA0\x80").error ==
           TextError::BadUtf8); // surrogate
    EXPECT(checkMessage("\xF4\x90\x80\x80").error == TextError::BadUtf8);
    EXPECT(checkMessage("\xE2\x82").error == TextError::BadUtf8); // truncated
    EXPECT(checkMessage("\xFF").error == TextError::BadUtf8);
    EXPECT(checkMessage("a\x80z").error == TextError::BadUtf8); // stray tail
}

TEST_CASE("Other scripts, emoji and look-alike signs are not allowed") {
    EXPECT(checkMessage("\xD0\x9F\xD1\x80\xD0\xB8").error ==
           TextError::Forbidden);
    EXPECT(checkMessage("smile \xF0\x9F\x98\x80").error ==
           TextError::Forbidden);
    EXPECT(checkMessage("a\xC3\x97"
                        "b")
               .error == TextError::Forbidden); // x sign
    EXPECT(checkMessage("a\xC3\xB7"
                        "b")
               .error == TextError::Forbidden); // / sign
    EXPECT(checkMessage("a\xC2\xA0"
                        "b")
               .error == TextError::Forbidden); // nbsp
}

TEST_CASE("Length counts characters, not bytes") {
    std::string s200(200, 'a');
    for (std::size_t i = 0; i < 200; i += 7) {
        s200[i] = 'b'; // No long runs.
    }
    EXPECT(valid(s200));
    EXPECT(checkMessage(s200 + "x").error == TextError::TooLong);
    std::string wide;
    for (int i = 0; i < 200; ++i) {
        wide += (i % 2) ? "\xC3\xA5" : "a"; // 300 bytes, 200 characters
    }
    EXPECT(valid(wide));
    EXPECT_EQ(checkMessage(wide).chars, std::size_t(200));
}

TEST_CASE("Empty, only spaces, edge spaces and repeated characters") {
    EXPECT(checkMessage("").error == TextError::Empty);
    EXPECT(checkMessage("    ").error == TextError::OnlySpaces);
    EXPECT(checkMessage(" a").error == TextError::EdgeSpace);
    EXPECT(checkMessage("a ").error == TextError::EdgeSpace);
    EXPECT(valid("a b  c"));
    EXPECT(valid("aaaaaaaa")); // 8 is allowed
    EXPECT(checkMessage("aaaaaaaaa").error == TextError::Repeated);
    EXPECT(checkMessage("hi" + std::string(9, '.')).error ==
           TextError::Repeated);
    // A draft may be empty, have edge spaces, and be only spaces while typing.
    EXPECT(valid("", false));
    EXPECT(valid("  hello ", false));
    EXPECT(valid("   ", false));
    EXPECT(!valid("\x1b", false));
    EXPECT(checkDraft(std::string(201, ' ')).error != TextError::None);
    EXPECT(trimSpaces("  hi there  ") == "hi there");
    EXPECT(trimSpaces("   ").empty());
}

TEST_CASE("Names") {
    EXPECT(checkName("ada") == NameError::None);
    EXPECT(checkName("a_b-9") == NameError::None);
    EXPECT(checkName("abcdefghijklmnop") == NameError::None); // 16
    EXPECT(checkName("a") == NameError::Length);
    EXPECT(checkName("abcdefghijklmnopq") == NameError::Length);
    EXPECT(checkName("a b") == NameError::Characters);
    EXPECT(checkName("\xC3\xA5sa") == NameError::Characters); // ASCII only
    EXPECT(checkName("a:b") == NameError::Characters);
    EXPECT(checkName("Admin") == NameError::Reserved);
    EXPECT(checkName("SERVER") == NameError::Reserved);
    EXPECT(checkName("mod_ann") == NameError::Reserved);
    EXPECT(checkName("you") == NameError::Reserved);
    EXPECT(nameKey("AdA_9") == "ada_9");
}

TEST_CASE("Incoming text is made safe and cannot form an escape sequence") {
    EXPECT(sanitizeIncoming("hello") == "hello");
    EXPECT(sanitizeIncoming("\x1b[31mred\x1b[0m") == "[31mred[0m");
    EXPECT(sanitizeIncoming("\x1b]52;c;AAAA\x07x") == "]52;c;AAAAx");
    EXPECT(sanitizeIncoming("a\xC2\x9B"
                            "31m") == "a31m");
    EXPECT(sanitizeIncoming("a\xE2\x80\xAE"
                            "b\xE2\x80\x8B"
                            "c") == "abc");
    EXPECT(sanitizeIncoming("a\r\nb\tc\x7f") == "abc");
    EXPECT(sanitizeIncoming("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT(sanitizeIncoming("\xD0\x9F") == "\xEF\xBF\xBD"); // Cyrillic
    EXPECT(sanitizeIncoming("smile \xF0\x9F\x98\x80!") ==
           "smile \xEF\xBF\xBD!");
    EXPECT(sanitizeIncoming("sm\xC3\xA5") == "sm\xC3\xA5");
    EXPECT(sanitizeIncoming(std::string(500, 'a'), 10) == std::string(10, 'a'));
    EXPECT(sanitizeIncoming("").empty());
}

TEST_CASE("Fuzz: whatever arrives, the output is clean, short and stable") {
    Lcg rng{12345};
    const char *seeds[] = {"\x1b[31m",
                           "\x1b]52;c;",
                           "\xC2\x9B",
                           "\xE2\x80\xAE",
                           "\xF0\x9F\x98\x80",
                           "\xED\xA0\x80",
                           "\xC3\xA5",
                           "\xFF",
                           "a",
                           " ",
                           "\x07",
                           "\xEF\xBB\xBF",
                           "\xF3\xA0\x80\x81"};
    for (int round = 0; round < 3000; ++round) {
        std::string input;
        const int parts = 1 + int(rng.next() % 12);
        for (int i = 0; i < parts; ++i) {
            if (rng.next() % 3 == 0) {
                input += char(rng.next() % 256);
            }
            else {
                input += seeds[rng.next() % (sizeof seeds / sizeof *seeds)];
            }
        }
        const std::size_t cap = 1 + rng.next() % 40;
        const std::string out = sanitizeIncoming(input, cap);
        EXPECT(clean(out));
        std::u32string cps;
        EXPECT(decodeUtf8Strict(out, cps));
        EXPECT(cps.size() <= cap);
        EXPECT(sanitizeIncoming(out, cap) == out);
        // What the server would accept never needs changing by the client.
        if (checkDraft(input).ok()) {
            EXPECT(sanitizeIncoming(input) == input);
        }
    }
}

TEST_CASE("Fuzz: anything accepted is clean, and checking is deterministic") {
    Lcg rng{777};
    for (int round = 0; round < 3000; ++round) {
        std::string input;
        const int n = int(rng.next() % 24);
        for (int i = 0; i < n; ++i) {
            const auto r = rng.next() % 6;
            input += r == 0   ? std::string(1, char(rng.next() % 256))
                     : r == 1 ? std::string("\xC3\xA5")
                              : std::string(1, char(32 + rng.next() % 95));
        }
        const auto result = checkMessage(input);
        EXPECT(checkMessage(input).error == result.error);
        if (result.ok()) {
            EXPECT(clean(input));
            EXPECT(input == trimSpaces(input));
            EXPECT(!input.empty());
        }
    }
}

TEST_SUIT_END
