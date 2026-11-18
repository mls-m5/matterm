#include "mls-unit-test/unittest.h"
#include "utf8decoder.h"

TEST_SUIT_BEGIN(Utf8Decoder)

TEST_CASE(
    "ASCII and Swedish text decode without platform conversion libraries") {
    Utf8Decoder decoder;
    EXPECT(decoder.feed("hello aåäö ÅÄÖ") == U"hello aåäö ÅÄÖ");
    EXPECT(decoder.finish().empty());
}

TEST_CASE("All UTF-8 lengths and scalar boundaries decode") {
    Utf8Decoder decoder;
    const std::string bytes =
        "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
        "\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf";
    const std::u32string expected{
        0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff};
    EXPECT(decoder.feed(bytes) == expected);
    EXPECT(decoder.finish().empty());
}

TEST_CASE("Characters split at any byte boundary survive incremental input") {
    const std::string bytes = "Aå€\xf0\x9f\x98\x80Z";
    const std::u32string expected = U"Aå€\U0001f600Z";
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        Utf8Decoder decoder;
        auto output = decoder.feed(std::string_view(bytes).substr(0, split));
        output += decoder.feed(std::string_view(bytes).substr(split));
        output += decoder.finish();
        EXPECT(output == expected);
    }
    Utf8Decoder decoder;
    std::u32string output;
    for (const char byte : bytes) {
        output += decoder.feed(std::string_view(&byte, 1));
    }
    EXPECT(output == expected);
}

TEST_CASE(
    "Malformed input rejects overlong surrogates and out-of-range scalars") {
    for (const std::string &bytes : {std::string("\xc0\xaf"),
                                     std::string("\xe0\x80\x80"),
                                     std::string("\xed\xa0\x80"),
                                     std::string("\xf4\x90\x80\x80"),
                                     std::string("\xff\x80")}) {
        Utf8Decoder decoder;
        auto output = decoder.feed(bytes);
        output += decoder.finish();
        EXPECT(!output.empty());
        for (const auto codePoint : output) {
            EXPECT(codePoint == U'\ufffd');
        }
    }
}

TEST_CASE("Invalid continuation reprocesses the next valid character") {
    Utf8Decoder decoder;
    EXPECT(decoder.feed("\xe2\x82").empty());
    EXPECT(decoder.feed("Aå") == U"\ufffdAå");
    EXPECT(decoder.finish().empty());
}

TEST_CASE(
    "Finish replaces a truncated prefix once and reset drops pending input") {
    Utf8Decoder decoder;
    EXPECT(decoder.feed("\xf0\x9f").empty());
    EXPECT(decoder.finish() == U"\ufffd");
    EXPECT(decoder.finish().empty());
    EXPECT(decoder.feed("\xc3").empty());
    decoder.reset();
    EXPECT(decoder.feed("ok") == U"ok");
    EXPECT(decoder.feed(std::string_view("\0a", 2)) ==
           std::u32string({U'\0', U'a'}));
}

TEST_SUIT_END
