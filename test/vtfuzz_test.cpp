#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

namespace {
constexpr unsigned seed = 0x5eed7102;
std::string hex(std::string_view bytes) {
    std::ostringstream out;
    for (unsigned char c : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
bool check(bool ok, unsigned trial, std::string_view bytes) {
    if (!ok)
        std::cerr << "seed=" << seed << " trial=" << trial
                  << " input(hex)=" << hex(bytes) << '\n';
    return ok;
}
bool invariant(const TerminalModel &model) {
    const auto cursor = model.cursor();
    const auto frame = model.frame(16);
    return cursor.column < model.columns() && cursor.row < model.rows() &&
           model.historySize() <= 24 &&
           (!model.alternateScreen() || model.historySize() == 0) &&
           frame.cells.rows() == model.rows() &&
           frame.cells.columns() == model.columns() && model.scrollTop() >= 0 &&
           model.scrollBottom() < int(model.rows()) &&
           model.scrollTop() <= model.scrollBottom();
}
std::vector<std::uint32_t> snapshot(TerminalModel &model) {
    std::vector<std::uint32_t> result;
    auto cell = [&](const FChar &value) {
        result.insert(result.end(),
                      {std::uint32_t(value.codePoint),
                       std::uint32_t(value.attributes),
                       value.foreground.r,
                       value.foreground.g,
                       value.foreground.b,
                       value.background.r,
                       value.background.g,
                       value.background.b});
    };
    const auto cursor = model.cursor();
    result.insert(result.end(),
                  {std::uint32_t(cursor.column),
                   std::uint32_t(cursor.row),
                   std::uint32_t(cursor.visible),
                   std::uint32_t(cursor.blink),
                   std::uint32_t(cursor.shape),
                   std::uint32_t(model.historySize()),
                   std::uint32_t(model.alternateScreen()),
                   std::uint32_t(model.scrollTop()),
                   std::uint32_t(model.scrollBottom()),
                   std::uint32_t(model.originMode())});
    for (std::size_t row = 0; row < model.rows(); ++row)
        for (std::size_t column = 0; column < model.columns(); ++column)
            cell(model.cell(column, row));
    // Inspect retained history as well as the live screen; followTail restores
    // the viewport without touching parser/model rendition state.
    model.scrollToOldest();
    model.advanceViewport(10);
    auto frame = model.frame(16);
    for (std::size_t row = 0; row < frame.cells.rows(); ++row)
        for (std::size_t column = 0; column < frame.cells.columns(); ++column)
            cell(frame.cells.row(row).data()[column]);
    model.followTail();
    model.advanceViewport(10);
    return result;
}
} // namespace
TEST_SUIT_BEGIN(VtFuzz)
TEST_CASE(
    "Random bytes and mutated transcripts are independent of feed boundaries") {
    const std::vector<std::string> tokens{
        "prompt$ echo hello\r\nhello\r\n",
        "\x1b[31;1mred\x1b[0m",
        "\x1b[38;2;12;34;56mRGB",
        "\x1b[2J\x1b[H",
        "\x1b[?1049h\x1b[3;5Hpanel\x1b[?1049l",
        "\x1b[2;5r\x1b[?6h",
        "\x1b[1000000A\x1b[1000000P",
        "\x1b[9999999999999999999C",
        "\x1b[1:2m",
        "\x1b[4hINSERT\x1b[4l",
        "\x1b[2L\x1b[3M\x1b[4X",
        "\x1b[2S\x1b[2T",
        "\x1b]2;fuzz title\x07",
        "\x1b]52;c;disabled\x1b\\",
        "\x1bPignored\x1b\\",
        "\x1b]2;" + std::string(2050, 'x') + "\x07",
        "\x1bP" + std::string(2050, 'x') + "\x1b\\",
        "\x1b[5n\x1b[6n",
        "\x1b(0lqk\x1b(B",
        "\x1b[?1006h\x1b[?2004h",
        "\x1b[!p",
        "\x1b"
        "7saved\x1b"
        "8",
        "\xc3\xa5\xe2\x94\x80\xf0\x9f\x98\x80",
        "\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80",
        "\xe2\x82",
        "\x1b[",
        "\x1b]2;unfinished",
        "\x18\x1a",
        "\t\b\r\n",
        "\x1b"
        "c"};
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 180; ++trial) {
        std::string bytes;
        if (trial < 60) {
            bytes.resize(384);
            for (auto &c : bytes)
                c = static_cast<char>(random());
        }
        else {
            for (unsigned i = 0; i < 16; ++i)
                bytes += tokens[random() % tokens.size()];
            if (trial % 3 == 0 && !bytes.empty())
                bytes[random() % bytes.size()] = static_cast<char>(random());
            if (trial % 5 == 0)
                bytes.resize(random() % (bytes.size() + 1));
        }
        TerminalModel whole(12, 6, 24), chunks(12, 6, 24), singles(12, 6, 24);
        std::string repliesA, repliesB, repliesC, titlesA, titlesB, titlesC;
        VtParser a(whole,
                   {[&](std::string_view s) { repliesA += s; },
                    [&](std::string_view s) { titlesA += s; }});
        VtParser b(chunks,
                   {[&](std::string_view s) { repliesB += s; },
                    [&](std::string_view s) { titlesB += s; }});
        VtParser c(singles,
                   {[&](std::string_view s) { repliesC += s; },
                    [&](std::string_view s) { titlesC += s; }});
        a.feed(bytes);
        for (std::size_t at = 0; at < bytes.size();) {
            const auto count =
                std::min<std::size_t>(1 + random() % 31, bytes.size() - at);
            b.feed(std::string_view(bytes).substr(at, count));
            EXPECT(check(invariant(chunks), trial, bytes));
            at += count;
        }
        for (std::size_t at = 0; at < bytes.size(); ++at) {
            c.feed(std::string_view(bytes).substr(at, 1));
            EXPECT(check(invariant(singles), trial, bytes));
        }
        a.finish();
        b.finish();
        c.finish();
        EXPECT(
            check(invariant(whole) && invariant(chunks) && invariant(singles),
                  trial,
                  bytes));
        const auto initial = snapshot(whole);
        EXPECT(
            check(initial == snapshot(chunks) && initial == snapshot(singles),
                  trial,
                  bytes));
        // A common suffix tests retained rendition, saved cursor and both
        // screens, not merely the visible state at EOF.
        const std::string suffix = "Z\r\n\x1b[?1049hALT\x1b[?1049l\x1b"
                                   "8Q";
        a.feed(suffix);
        b.feed(suffix);
        c.feed(suffix);
        a.finish();
        b.finish();
        c.finish();
        const auto expected = snapshot(whole);
        EXPECT(
            check(expected == snapshot(chunks) && expected == snapshot(singles),
                  trial,
                  bytes));
        EXPECT(check(repliesA == repliesB && repliesA == repliesC &&
                         titlesA == titlesB && titlesA == titlesC,
                     trial,
                     bytes));
    }
}
TEST_SUIT_END
