#include "control.h"
#include "framing.h"
#include "mls-unit-test/unittest.h"

namespace {
const std::string firstId(32, 'a'), secondId(32, 'b');
mtd::SessionDetail row(const std::string &id = firstId) {
    mtd::SessionDetail r;
    r.id = id;
    r.name = "build";
    r.cwd = "/home/u/project";
    r.command = {"nvim", "file"};
    r.foregroundBusy = true;
    r.outputAgeMs = 123;
    r.preview = "å服务器 recent output";
    return r;
}
} // namespace
TEST_SUIT_BEGIN(SessionDetails)
TEST_CASE("Details request is bounded and rejects duplicates, invalid ids and "
          "malformed flags") {
    std::string bytes;
    EXPECT(mtd::encode(
        mtd::ControlRequest(mtd::GetSessionDetails{{firstId, secondId}, true}),
        bytes));
    const auto request = mtd::decodeRequest(bytes);
    EXPECT(request.has_value());
    const auto &r = std::get<mtd::GetSessionDetails>(*request);
    EXPECT(r.ids.size() == 2 && r.includePreview);
    EXPECT(!mtd::decodeRequest(bytes + "x"));
    auto flags = bytes;
    flags[2] = 2;
    EXPECT(!mtd::decodeRequest(flags));
    bytes.pop_back();
    EXPECT(!mtd::decodeRequest(bytes));
    bytes.clear();
    EXPECT(!mtd::encode(
        mtd::ControlRequest(mtd::GetSessionDetails{{firstId, firstId}, true}),
        bytes));
    EXPECT(bytes.empty());
    EXPECT(!mtd::encode(
        mtd::ControlRequest(mtd::GetSessionDetails{{"bad"}, true}), bytes));
    EXPECT(!mtd::encode(mtd::ControlRequest(mtd::GetSessionDetails{{}, false}),
                        bytes));
    std::vector<std::string> ids;
    for (int i = 0; i < 17; ++i) {
        auto id = firstId;
        id[0] = char('a' + i % 6);
        id[1] = char('0' + i / 6);
        ids.push_back(id);
    }
    EXPECT(!mtd::encode(mtd::ControlRequest(mtd::GetSessionDetails{ids, false}),
                        bytes));
}
TEST_CASE("Details presence distinguishes unknown, known empty, shell "
          "foreground and busy") {
    mtd::SessionDetails details{2, {row(), row(secondId)}};
    details.rows[1].foregroundBusy = false;
    details.rows[1].outputAgeMs.reset();
    details.rows[1].preview = "";
    std::string bytes;
    EXPECT(mtd::encode(mtd::ControlReply(details), bytes));
    EXPECT(bytes.size() <= mtd::kMaxFrame);
    const auto decoded = mtd::decodeReply(bytes);
    EXPECT(decoded.has_value());
    const auto &d = std::get<mtd::SessionDetails>(*decoded);
    EXPECT(d.processedCount == 2 && d.rows.size() == 2);
    EXPECT(d.rows[0].foregroundBusy == true && d.rows[0].outputAgeMs == 123);
    EXPECT(d.rows[0].preview == details.rows[0].preview);
    EXPECT(d.rows[1].foregroundBusy == false && !d.rows[1].outputAgeMs);
    EXPECT(d.rows[1].preview.has_value() && d.rows[1].preview->empty());
    auto flags = bytes;
    flags[38] =
        char(0x80); // Header4 + encoded32-byte ID34, then presence flags.
    EXPECT(!mtd::decodeReply(flags));
    flags[38] = 2; // Busy cannot be known true without foreground-known.
    EXPECT(!mtd::decodeReply(flags));
    details.rows[0].foregroundBusy.reset();
    details.rows[0].preview.reset();
    bytes.clear();
    EXPECT(mtd::encode(mtd::ControlReply(details), bytes));
    EXPECT(!mtd::decodeReply(bytes + "x"));
    for (std::size_t n = 0; n < bytes.size(); ++n)
        EXPECT(!mtd::decodeReply(std::string_view(bytes).substr(0, n)));
}
TEST_CASE(
    "Preview text has strict UTF8 scalar and control bounds, including bidi") {
    EXPECT(mtd::validPreviewText("å服务器"));
    EXPECT(mtd::validPreviewText(""));
    for (const std::string &unsafe : {std::string("\x1b]52;c;hidden\a"),
                                      std::string("a\nb"),
                                      std::string("\xc2\x9b"),
                                      std::string("\xe2\x80\xae"),
                                      std::string("\xd8\x9c"),
                                      std::string("\xc3")})
        EXPECT(!mtd::validPreviewText(unsafe));
    EXPECT(mtd::validPreviewText(std::string(256, 'x')));
    EXPECT(!mtd::validPreviewText(std::string(257, 'x')));
    std::string emoji;
    for (int i = 0; i < 256; ++i)
        emoji += "🔥";
    EXPECT(emoji.size() == 1024);
    EXPECT(mtd::validPreviewText(emoji));
    auto invalid = row();
    invalid.preview = "\x1b[31mred";
    std::string bytes;
    EXPECT(!mtd::encode(mtd::ControlReply(mtd::SessionDetails{1, {invalid}}),
                        bytes));
    EXPECT(bytes.empty());
}
TEST_CASE("Details enforce the 16KiB frame and aggregate command caps without "
          "truncating encoder output") {
    auto large = row();
    large.cwd = std::string(4096, '/');
    large.command = {std::string(1023, 'a'),
                     std::string(1023, 'b'),
                     std::string(1023, 'c'),
                     std::string(1023, 'd')};
    large.preview = std::string(256, 'x');
    std::string bytes;
    EXPECT(
        mtd::encode(mtd::ControlReply(mtd::SessionDetails{1, {large}}), bytes));
    EXPECT(bytes.size() < mtd::kMaxFrame);
    auto second = large;
    second.id = secondId;
    std::string unchanged = "prefix";
    EXPECT(!mtd::encode(
        mtd::ControlReply(mtd::SessionDetails{2, {large, second}}), unchanged));
    EXPECT(unchanged == "prefix");
    large.command.push_back("x");
    bytes.clear();
    EXPECT(!mtd::encode(mtd::ControlReply(mtd::SessionDetails{1, {large}}),
                        bytes));
    EXPECT(!mtd::encode(mtd::ControlReply(mtd::SessionDetails{0, {row()}}),
                        bytes));
}
TEST_SUIT_END
