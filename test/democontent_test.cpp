#include "democontent.h"
#include "mls-unit-test/unittest.h"
#include "terminalmemory.h"

#include <sstream>
#include <string>

TEST_SUIT_BEGIN(DemoContent)

TEST_CASE("Startup prints an ASCII banner with the lowercase matterm name") {
    std::ostringstream output;
    demo::printBanner(output);
    const auto text = output.str();
    EXPECT(text.find("matterm") != std::string::npos);
    EXPECT(text.find("_| |_| |_|") != std::string::npos);
    for (const unsigned char ch : text) {
        EXPECT(ch < 128);
    }
}

TEST_CASE(
    "Welcome screen starts with matterm and restores cell color gradients") {
    TerminalMemory memory(64, 256, 16);
    demo::welcome(memory, 22);
    EXPECT_EQ(memory.size(), std::size_t(22));
    const auto title = memory.row(0);
    EXPECT(title.data()[1].codePoint == U'm');
    EXPECT(title.data()[7].codePoint == U'm');
    const auto backgrounds = memory.row(10);
    EXPECT_EQ(backgrounds.data()[1].background.r, 40);
    EXPECT_EQ(backgrounds.data()[48].background.r, 220);
    EXPECT_NE(backgrounds.data()[1].background.b,
              backgrounds.data()[48].background.b);
    const auto foregrounds = memory.row(11);
    EXPECT(foregrounds.data()[1].codePoint == U'#');
    EXPECT_EQ(foregrounds.data()[48].foreground.r, 220);
    EXPECT_EQ(foregrounds.data()[48].background.r, 14);
}

TEST_CASE("Numbered output evicts welcome rows and welcome can be restored") {
    TerminalMemory memory(64, 256, 16);
    demo::welcome(memory, 22);
    for (std::uint64_t i = 1; i <= 300; ++i) {
        demo::appendNumberedRow(memory, i);
    }
    EXPECT_EQ(memory.size(), std::size_t(256));
    EXPECT(memory.row(0).data()[0].codePoint == U'R');
    EXPECT(memory.row(0).data()[4].codePoint == U'4');
    EXPECT(memory.row(0).data()[5].codePoint == U'5');
    demo::welcome(memory, 22);
    EXPECT_EQ(memory.size(), std::size_t(22));
    EXPECT(memory.row(0).data()[1].codePoint == U'm');
    memory.resizeColumns(80);
    demo::welcome(memory, 25);
    EXPECT_EQ(memory.row(0).size(), std::size_t(80));
    EXPECT_EQ(memory.size(), std::size_t(25));
}

TEST_CASE("Welcome tolerates tiny windows and capacities") {
    TerminalMemory memory(0, 1, 1);
    demo::welcome(memory, 0);
    EXPECT_EQ(memory.size(), std::size_t(1));
    EXPECT_EQ(memory.row(0).size(), std::size_t(0));
    memory.resizeColumns(1);
    demo::welcome(memory, 1000);
    EXPECT_EQ(memory.size(), std::size_t(1));
    EXPECT_EQ(memory.row(0).size(), std::size_t(1));
}

TEST_SUIT_END
