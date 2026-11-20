#include "chatproto.h"
#include "chattext.h"
#include "mls-unit-test/unittest.h"

#include <cstdint>
#include <string>

namespace {
using namespace chat;

ClientLine hello(const std::string &name) {
    ClientLine l;
    l.kind = ClientLine::Kind::Hello;
    l.name = name;
    return l;
}
ClientLine draft(std::uint32_t b, const std::string &text) {
    ClientLine l;
    l.kind = ClientLine::Kind::Draft;
    l.bubble = b;
    l.text = text;
    return l;
}
ClientLine lock(std::uint32_t b) {
    ClientLine l;
    l.kind = ClientLine::Kind::Lock;
    l.bubble = b;
    return l;
}

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return std::uint32_t(s >> 33);
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatProto)

TEST_CASE("Client lines have exactly one text form") {
    EXPECT(formatClientLine(hello("ada")) == "hello\tada");
    EXPECT(formatClientLine(draft(7, "hi there")) == "draft\t7\thi there");
    EXPECT(formatClientLine(draft(7, "")) == "draft\t7\t");
    EXPECT(formatClientLine(lock(7)) == "lock\t7");
    ClientLine nick;
    nick.kind = ClientLine::Kind::Nick;
    nick.name = "bo";
    EXPECT(formatClientLine(nick) == "nick\tbo");
    for (const auto &line : {hello("ada"),
                             draft(1, " a b "),
                             draft(999999999, ""),
                             lock(42),
                             nick}) {
        const auto parsed = parseClientLine(formatClientLine(line));
        EXPECT(parsed.error == ProtoError::None);
        EXPECT(parsed.line == line);
    }
}

TEST_CASE("Client lines that are not exactly right are errors") {
    auto bad = [](const std::string &frame, ProtoError expected) {
        EXPECT(parseClientLine(frame).error == expected);
    };
    bad("", ProtoError::Unknown);
    bad("hi\tada", ProtoError::Unknown);
    bad("HELLO\tada", ProtoError::Unknown);
    bad("hello", ProtoError::Fields);
    bad("hello\tada\textra", ProtoError::Fields);
    bad("hello\ta", ProtoError::Name);
    bad("hello\tAdmin", ProtoError::Name);
    bad("hello\ta b", ProtoError::Name);
    bad("draft\t1", ProtoError::Fields);
    bad("draft\t1\ta\tb", ProtoError::Fields);
    bad("draft\t0\thi", ProtoError::Number);
    bad("draft\t01\thi", ProtoError::Number);
    bad("draft\t-1\thi", ProtoError::Number);
    bad("draft\t+1\thi", ProtoError::Number);
    bad("draft\t1000000000\thi", ProtoError::Number);
    bad("draft\tx\thi", ProtoError::Number);
    bad("draft\t \thi", ProtoError::Number);
    bad("draft\t1\t\x1b[31m", ProtoError::BadChars);
    bad("draft\t1\t\xE2\x80\xAE"
        "x",
        ProtoError::Text);
    bad("draft\t1\t" + std::string(201, 'a'), ProtoError::Text);
    bad("draft\t1\taaaaaaaaaaaaaaaa", ProtoError::Text);
    bad("lock", ProtoError::Fields);
    bad("lock\t1\t2", ProtoError::Fields);
    bad("lock\t0", ProtoError::Number);
    bad("hello\tada\n", ProtoError::BadChars);
    bad("hello\tada\r", ProtoError::BadChars);
    bad(std::string("hello\tada\0", 10), ProtoError::BadChars);
    bad("draft\t1\t" + std::string(1100, 'a'), ProtoError::TooLong);
}

TEST_CASE("Server lines round trip") {
    std::vector<ServerLine> lines;
    auto add = [&](ServerLine::Kind k) {
        lines.emplace_back();
        lines.back().kind = k;
        return &lines.back();
    };
    add(ServerLine::Kind::Welcome)->name = "ada";
    add(ServerLine::Kind::Users)->names = {"bo", "cy", "dee"};
    add(ServerLine::Kind::Users)->names = {};
    {
        auto *l = add(ServerLine::Kind::Hist);
        l->serial = 4812;
        l->age = 61;
        l->name = "bo";
        l->text = "hi everyone";
    }
    add(ServerLine::Kind::Ready);
    {
        auto *l = add(ServerLine::Kind::Draft);
        l->name = "cy";
        l->text = "ok but wh";
    }
    add(ServerLine::Kind::Gone)->name = "cy";
    add(ServerLine::Kind::Typers)->count = 3;
    {
        auto *l = add(ServerLine::Kind::Msg);
        l->serial = 4813;
        l->name = "cy";
        l->text = "hej! sm\xC3\xA5";
    }
    {
        auto *l = add(ServerLine::Kind::Ack);
        l->bubble = 7;
        l->serial = 4813;
        l->text = "hej";
    }
    add(ServerLine::Kind::Online)->count = 12;
    add(ServerLine::Kind::Join)->name = "dee";
    add(ServerLine::Kind::Leave)->name = "dee";
    add(ServerLine::Kind::Hide)->serial = 4812;
    add(ServerLine::Kind::Notice)->text = "the room is closing soon";
    {
        auto *l = add(ServerLine::Kind::Error);
        l->code = ErrorCode::NameTaken;
        l->text = "that name is in use";
    }
    for (const auto &line : lines) {
        const std::string frame = formatServerLine(line);
        EXPECT(frame.size() <= kMaxFrameBytes);
        const auto parsed = parseServerLine(frame);
        EXPECT(parsed.error == ProtoError::None);
        EXPECT(parsed.line == line);
        EXPECT(formatServerLine(parsed.line) == frame);
    }
    EXPECT(formatServerLine(lines[1]) == "users\tbo,cy,dee");
    EXPECT(formatServerLine(lines[3]) == "hist\t4812\t61\tbo\thi everyone");
}

TEST_CASE("Server lines: structure is strict, text is made safe") {
    auto err = [](const std::string &frame) {
        return parseServerLine(frame).error;
    };
    EXPECT(err("") == ProtoError::Unknown);
    EXPECT(err("bogus\tx") == ProtoError::Unknown);
    EXPECT(err("ready\tx") == ProtoError::Fields);
    EXPECT(err("msg\t1\tbo") == ProtoError::Fields);
    EXPECT(err("msg\tx\tbo\thi") == ProtoError::Number);
    EXPECT(err("msg\t1\tb o\thi") == ProtoError::Name);
    EXPECT(err("msg\t1\tserver\thi") == ProtoError::Name);
    EXPECT(err("users\tbo,,cy") == ProtoError::Name);
    EXPECT(err("online\t-3") == ProtoError::Number);
    EXPECT(err("error\tnope\ttext") == ProtoError::Unknown);
    EXPECT(err("msg\t1\tbo\thi\n") == ProtoError::BadChars);
    EXPECT(err(std::string(2000, 'x')) == ProtoError::TooLong);
    // Control bytes make the whole frame an error; other hostile text arrives
    // clean, in the same shape.
    EXPECT(err("notice\t\x1b[2Jhello") == ProtoError::BadChars);
    EXPECT(err("msg\t5\tbo\t\x1b]52;c;AAAA\x07") == ProtoError::BadChars);
    const auto p = parseServerLine("msg\t5\tbo\t\xC2\x9B"
                                   "31m \xE2\x80\xAE"
                                   "x\xF0\x9F\x98\x80");
    EXPECT(p.error == ProtoError::None);
    EXPECT(p.line.text == "31m x\xEF\xBF\xBD");
    EXPECT(parseServerLine("notice\t\xFF"
                           "hello")
               .line.text == "\xEF\xBF\xBD"
                             "hello");
}

TEST_CASE("Fuzz: parsing never misbehaves and accepted lines are canonical") {
    Lcg rng{2024};
    const char *pieces[] = {
        "hello",        "draft",     "lock",       "nick", "msg",      "users",
        "\t",           "\t",        "\t",         "7",    "0",        "ada",
        "a b",          "\x1b",      "\n",         "\xFF", "\xC3\xA5", ",",
        "aaaaaaaaaaaa", "999999999", "1000000000", " "};
    for (int round = 0; round < 20000; ++round) {
        std::string frame;
        const int n = 1 + int(rng.next() % 8);
        for (int i = 0; i < n; ++i) {
            frame += pieces[rng.next() % (sizeof pieces / sizeof *pieces)];
        }
        const auto c = parseClientLine(frame);
        if (c.error == ProtoError::None) {
            EXPECT(formatClientLine(c.line) == frame);
        }
        const auto s = parseServerLine(frame);
        if (s.error == ProtoError::None) {
            const auto again = parseServerLine(formatServerLine(s.line));
            EXPECT(again.error == ProtoError::None);
            EXPECT(again.line == s.line);
            std::u32string cps;
            EXPECT(decodeUtf8Strict(s.line.text, cps));
        }
    }
}

TEST_SUIT_END
