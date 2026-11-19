#include "mls-unit-test/unittest.h"
#include "urllink.h"

#include <limits>
#include <string>

namespace {
using frontend::urlAt;
using frontend::validHttpUrl;

std::string detected(std::u32string_view input, std::size_t column = 2) {
    TerminalModel model(input.size() + 2, 2);
    model.write(input);
    const auto link = urlAt(model, {0, column});
    return link ? link->text : "<none>";
}
} // namespace

TEST_SUIT_BEGIN(UrlLink)

TEST_CASE("Detect plain http and https URLs under any interior cell") {
    TerminalModel model(80, 2);
    model.write(U"see https://example.test/path now");
    for (std::size_t column = 4; column < 29; ++column) {
        const auto link = urlAt(model, {0, column});
        EXPECT(bool(link));
        EXPECT_EQ(link->text, "https://example.test/path");
        EXPECT_EQ(link->cells.anchor.column, 4u);
        EXPECT_EQ(link->cells.end.column, 28u);
    }
    EXPECT(!urlAt(model, {0, 3}));
    EXPECT(!urlAt(model, {0, 29}));
    EXPECT(!urlAt(model, {0, 80}));
    EXPECT(!urlAt(model, {std::numeric_limits<std::size_t>::max(), 0}));
    EXPECT_EQ(detected(U"http://localhost:8080/"), "http://localhost:8080/");
    EXPECT_EQ(detected(U"HTTPS://EXAMPLE.TEST/"), "HTTPS://EXAMPLE.TEST/");
}

TEST_CASE("Query fragment and percent encoded paths remain intact") {
    EXPECT_EQ(detected(U"https://example.test/a_b?q=a%20b&x=1#section"),
              "https://example.test/a_b?q=a%20b&x=1#section");
    EXPECT(validHttpUrl("http://127.0.0.1:8000/a"));
    EXPECT(validHttpUrl("https://[2001:db8::1]:443/a(b)"));
    EXPECT(validHttpUrl("http://[::1]/"));
    EXPECT(validHttpUrl("https://1234.example/a/%C3%A5"));
}

TEST_CASE(
    "Soft wrapped URLs retain absolute cell ranges and stop at hard newlines") {
    TerminalModel model(8, 5, 10);
    model.write(U"https://example.test/path");
    const auto link = urlAt(model, {1, 3});
    EXPECT(bool(link));
    EXPECT_EQ(link->text, "https://example.test/path");
    EXPECT_EQ(link->cells.anchor.row, 0u);
    EXPECT_EQ(link->cells.end.row, 3u);
    EXPECT_EQ(link->cells.end.column, 0u);
    TerminalModel offset(8, 6);
    offset.write(U"go https://example.test/path.");
    const auto shifted = urlAt(offset, {2, 1});
    EXPECT(bool(shifted));
    EXPECT_EQ(shifted->text, "https://example.test/path");
    EXPECT_EQ(shifted->cells.anchor.column, 3u);
    EXPECT_EQ(shifted->cells.end.row, 3u);
    EXPECT_EQ(shifted->cells.end.column, 3u);
    EXPECT(!urlAt(offset, {3, 4}));
    TerminalModel hard(80, 2);
    hard.write(U"https://example.test/\r\npath");
    EXPECT_EQ(urlAt(hard, {0, 2})->text, "https://example.test/");
    EXPECT(!urlAt(hard, {1, 2}));
}

TEST_CASE("Detection works in history but never joins across evicted text") {
    TerminalModel model(8, 2, 6);
    model.write(U"https://example.test/path\r\nnext\r\nlast");
    auto link = urlAt(model, {0, 2});
    EXPECT(bool(link));
    EXPECT_EQ(link->text, "https://example.test/path");
    model.write(U"\r\na\r\nb\r\nc\r\nd\r\ne");
    EXPECT_GT(model.firstRetainedRow(), 0u);
    EXPECT(!urlAt(model, {0, 2}));
    EXPECT(!urlAt(model, {model.firstRetainedRow(), 2}));
}

TEST_CASE("Trim prose punctuation and only unmatched closing brackets") {
    EXPECT_EQ(detected(U"(https://example.test/a).,;:!?)]}>\"'", 5),
              "https://example.test/a");
    EXPECT_EQ(detected(U"https://example.test/a(b)."),
              "https://example.test/a(b)");
    EXPECT_EQ(detected(U"[https://example.test/[x]]", 5),
              "https://example.test/[x]");
    EXPECT_EQ(detected(U"\"https://example.test/a\"", 5),
              "https://example.test/a");
    EXPECT_EQ(detected(U"<https://example.test/a>", 5),
              "https://example.test/a");
    TerminalModel model(80, 2);
    model.write(U"(https://example.test). ");
    EXPECT(!urlAt(model, {0, 0}));
    EXPECT(!urlAt(model, {0, 21}));
    EXPECT(!urlAt(model, {0, 22}));
}

TEST_CASE("Non-http schemes and misleading scheme prefixes are rejected") {
    for (const auto input : {U"javascript:alert(1)",
                             U"data:text/html,hello",
                             U"file:///tmp/test",
                             U"mailto:a@example.test",
                             U"ftp://example.test/",
                             U"javascript:https://example.test/",
                             U"prefixhttps://example.test/",
                             U"//example.test/"}) {
        EXPECT_EQ(detected(input), "<none>");
    }
}

TEST_CASE("Malformed authorities ports and percent escapes are rejected") {
    for (const auto url : {"",
                           "http://",
                           "https:///path",
                           "https://?q=x",
                           "https://#x",
                           "https://-host/",
                           "https://a..b/",
                           "https://host:abc/",
                           "https://host:0/",
                           "https://host:65536/",
                           "https://host:/",
                           "https://host:80:90/",
                           "https://user:password@host/",
                           "http://999.1.2.3/",
                           "http://127.1/",
                           "http://0177.0.0.1/",
                           "http://0x7f000001/",
                           "http://0x7f.0.0.1/",
                           "https://[]/",
                           "https://[::::]/",
                           "https://[1:2:3]/",
                           "https://[::1]extra/",
                           "https://host/%",
                           "https://host/%G0"}) {
        EXPECT(!validHttpUrl(url));
    }
}

TEST_CASE(
    "Embedded quotes backslashes and controls are never passed to an opener") {
    for (const auto url : {"https://host/a\"b",
                           "https://host/a'b",
                           "https://host/a\\b",
                           "https://good\\@evil/",
                           "https://host/a\nb",
                           "https://host/a\rb",
                           "https://host/a\tb",
                           "https://host/\x1b[31m",
                           "https://host/\x7f",
                           "https://host/%00",
                           "https://host/%0a",
                           "https://host/%22",
                           "https://host/%27",
                           "https://host/%5c"}) {
        EXPECT(!validHttpUrl(url));
    }
    EXPECT(!validHttpUrl(std::string("https://host/a\0b", 16)));
    EXPECT(!validHttpUrl("https://host/\xc2\x85"));
    EXPECT_EQ(detected(U"https://host/a\"b"), "<none>");
    EXPECT_EQ(detected(U"https://host/a'b"), "<none>");
    EXPECT_EQ(detected(U"https://host/a\\b"), "<none>");
    EXPECT_EQ(detected(U"https://host/%0a"), "<none>");
}

TEST_CASE("Exact 2048 byte URLs are allowed but longer tokens are bounded and "
          "rejected") {
    const std::string base = "https://example.test/";
    const auto exact = base + std::string(2048 - base.size(), 'a');
    EXPECT(validHttpUrl(exact));
    EXPECT(!validHttpUrl(exact + "a"));
    TerminalModel model(64, 40);
    model.write(std::u32string(exact.begin(), exact.end()));
    auto link = urlAt(model, {10, 10});
    EXPECT(bool(link));
    EXPECT_EQ(link->text, exact);
    model.write(U"a");
    EXPECT(!urlAt(model, {10, 10}));
    TerminalModel huge(80, 70);
    huge.write(U"https://");
    huge.write(std::u32string(5000, U'a'));
    EXPECT(!urlAt(huge, {0, 3}));
    EXPECT(!urlAt(huge, {30, 3}));
    EXPECT(!urlAt(huge, {60, 3}));
}

TEST_CASE("Hover underline clips like selection without changing cells or "
          "selection") {
    TerminalModel model(8, 2, 4);
    model.write(U"https://example.test/path");
    auto link = urlAt(model, {0, 1});
    EXPECT(bool(link));
    auto frame = model.frame(10);
    frame.selectionBegin = 2;
    frame.selectionEnd = 4;
    link->underline(model, frame);
    EXPECT_EQ(frame.underlineBegin, 0u);
    EXPECT_EQ(frame.underlineEnd, 9u);
    EXPECT_EQ(frame.selectionBegin, 2u);
    EXPECT_EQ(frame.selectionEnd, 4u);
    EXPECT(
        !hasAttribute(model.cell(0, 0).attributes, CellAttribute::Underline));
    model.resize(9, 2);
    frame = model.frame(10);
    link->underline(model, frame);
    EXPECT_EQ(frame.underlineBegin, 0u);
    EXPECT_EQ(frame.underlineEnd, 0u);
}

TEST_CASE("Link gestures require Ctrl and defer to application mouse capture") {
    EXPECT(!frontend::localLinkGesture(false, false, false));
    EXPECT(frontend::localLinkGesture(true, false, false));
    EXPECT(!frontend::localLinkGesture(true, false, true));
    EXPECT(frontend::localLinkGesture(true, true, true));
    EXPECT(!frontend::localLinkGesture(true, true, true, true));
    EXPECT(!frontend::localLinkGesture(true, false, false, true));
}

TEST_CASE(
    "Ordinary text empty cells and unencoded non-ASCII URLs are not links") {
    EXPECT_EQ(detected(U"ordinary text"), "<none>");
    EXPECT_EQ(detected(U"https://exämple.test/"), "<none>");
    EXPECT_EQ(detected(U"https://example.test/å"), "<none>");
    TerminalModel model(10, 2);
    EXPECT(!urlAt(model, {0, 0}));
    EXPECT(!urlAt(model, {1, 9}));
}

TEST_SUIT_END
