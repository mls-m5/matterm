#include "message.h"
#include "mls-unit-test/unittest.h"

#include <random>

using namespace protocol;

namespace {
// Builds a byte string from hexadecimal text such as "01 00 01".
std::string hex(std::string_view text) {
    std::string result;
    int high = -1;
    for (const char ch : text) {
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        }
        else if (ch >= 'a' && ch <= 'f') {
            digit = ch - 'a' + 10;
        }
        if (digit < 0) {
            continue;
        }
        if (high < 0) {
            high = digit;
        }
        else {
            result.push_back(char(high * 16 + digit));
            high = -1;
        }
    }
    return result;
}

std::string encoded(const ClientMessage &message) {
    std::string out;
    return encode(message, out) ? out : std::string("<encode failed>");
}
std::string encoded(const ServerMessage &message) {
    std::string out;
    return encode(message, out) ? out : std::string("<encode failed>");
}
template <typename T, typename Variant>
const T &as(const Variant &variant) {
    return std::get<T>(variant);
}
} // namespace

TEST_SUIT_BEGIN(Protocol)

// The vectors below are the worked examples of docs/PROTOCOL.md.
TEST_CASE("Golden vectors encode and decode exactly as documented") {
    Hello hello;
    const auto helloBytes = hex("01 0001 0050 0018 0000000000000000 0000 "
                                "00000000");
    EXPECT(encoded(hello) == helloBytes);
    const auto decodedHello = decodeClient(helloBytes);
    EXPECT(decodedHello);
    EXPECT_EQ(as<Hello>(*decodedHello.message).columns, 80);
    EXPECT_EQ(as<Hello>(*decodedHello.message).rows, 24);

    Welcome welcome;
    welcome.sessionId = "ab12";
    EXPECT(encoded(welcome) == hex("81 0001 0004 61623132 0000000000000000 "
                                   "00 00000000"));
    EXPECT(encoded(Input{"ls\r"}) == hex("02 6c 73 0d"));
    EXPECT(encoded(Output{0, "ls\r\n"}) ==
           hex("82 0000000000000000 6c 73 0d 0a"));
    EXPECT(encoded(Output{4, "a.txt\r\n"}) ==
           hex("82 0000000000000004 61 2e 74 78 74 0d 0a"));
    EXPECT(encoded(Ack{11}) == hex("04 000000000000000b"));
    Resize resize;
    resize.columns = 120;
    resize.rows = 40;
    EXPECT(encoded(resize) == hex("03 0078 0028 0000 0000"));
    EXPECT(encoded(Exit{0, 0}) == hex("83 00000000 00000000"));
    EXPECT(encoded(Ping{0x01020304}) == hex("05 01020304"));
    EXPECT(encoded(Pong{0x01020304}) == hex("84 01020304"));
    EXPECT(encoded(Close{}) == hex("07"));
    EXPECT(encoded(Error{ErrorCode::BadMessage, "no"}) ==
           hex("85 0001 0002 6e 6f"));
}

TEST_CASE("Every message round-trips through its decoder") {
    Hello hello;
    hello.version = 7;
    hello.columns = 1000;
    hello.rows = 1;
    hello.resumeOffset = 0x0102030405060708ull;
    hello.sessionId = "Åsa-ü";
    hello.features = 0x80000001u;
    const auto helloBytes = encoded(hello);
    const auto h = decodeClient(helloBytes);
    EXPECT(h);
    EXPECT(as<Hello>(*h.message).resumeOffset == hello.resumeOffset);
    EXPECT(as<Hello>(*h.message).sessionId == hello.sessionId);
    EXPECT_EQ(as<Hello>(*h.message).features, hello.features);
    EXPECT_EQ(as<Hello>(*h.message).version, 7);

    Resize resize{3, 4, 5, 6};
    const auto r = decodeClient(encoded(resize));
    EXPECT(r);
    EXPECT_EQ(as<Resize>(*r.message).pixelHeight, 6);

    const auto ack = decodeClient(encoded(Ack{0xffffffffffffffffull}));
    EXPECT(ack);
    EXPECT(as<Ack>(*ack.message).consumedOffset == 0xffffffffffffffffull);

    const auto exit = decodeServer(encoded(Exit{-1, 9}));
    EXPECT(exit);
    EXPECT_EQ(as<Exit>(*exit.message).code, -1);
    EXPECT_EQ(as<Exit>(*exit.message).signal, 9);

    Welcome welcome{1, "id", 5, 3, 2};
    const auto w = decodeServer(encoded(welcome));
    EXPECT(w);
    EXPECT_EQ(as<Welcome>(*w.message).flags, 3);
    EXPECT_EQ(as<Welcome>(*w.message).startOffset, 5);

    const auto o = decodeServer(encoded(Output{1ull << 40, "\x1b[31mred"}));
    EXPECT(o);
    EXPECT(as<Output>(*o.message).offset == (1ull << 40));
    EXPECT(as<Output>(*o.message).data == "\x1b[31mred");

    const auto e =
        decodeServer(encoded(Error{ErrorCode::Internal, "Något gick fel"}));
    EXPECT(e);
    EXPECT(as<Error>(*e.message).code == ErrorCode::Internal);
    EXPECT(decodeClient(encoded(Close{})));
}

TEST_CASE("Size limits are exact at the boundary") {
    std::string out;
    EXPECT(encode(Input{std::string(maxInputData, 'x')}, out));
    EXPECT_EQ(out.size(), maxMessageSize);
    EXPECT(decodeClient(out));
    out.clear();
    EXPECT_FALSE(encode(Input{std::string(maxInputData + 1, 'x')}, out));
    EXPECT(out.empty());
    EXPECT_FALSE(encode(Input{}, out));

    EXPECT(encode(Output{1, std::string(maxOutputData, 'x')}, out));
    EXPECT_EQ(out.size(), maxMessageSize);
    EXPECT(decodeServer(out));
    out.clear();
    EXPECT_FALSE(encode(Output{1, std::string(maxOutputData + 1, 'x')}, out));
    EXPECT_FALSE(encode(Output{1, ""}, out));

    EXPECT(decodeClient(std::string(maxMessageSize, '\x02')));
    const auto big = decodeClient(std::string(maxMessageSize + 1, '\x02'));
    EXPECT(!big);
    EXPECT(big.error == DecodeError::TooLarge);
    EXPECT(decodeServer(std::string(maxMessageSize + 1, '\x82')).error ==
           DecodeError::TooLarge);
}

TEST_CASE("Out of range values are rejected when encoding and decoding") {
    std::string out;
    Hello hello;
    hello.columns = 0;
    EXPECT_FALSE(encode(hello, out));
    hello.columns = 1001;
    EXPECT_FALSE(encode(hello, out));
    hello.columns = 80;
    hello.sessionId = std::string(maxSessionIdSize + 1, 'a');
    EXPECT_FALSE(encode(hello, out));
    hello.sessionId = std::string(maxSessionIdSize, 'a');
    EXPECT(encode(hello, out));
    EXPECT_FALSE(encode(Resize{0, 24, 0, 0}, out));
    EXPECT_FALSE(encode(Resize{80, 1001, 0, 0}, out));
    EXPECT_FALSE(encode(Error{ErrorCode(0), "x"}, out));
    EXPECT_FALSE(encode(Error{ErrorCode(11), "x"}, out));
    EXPECT_FALSE(
        encode(Error{ErrorCode::Internal, std::string(257, 'x')}, out));
    EXPECT_FALSE(encode(Error{ErrorCode::Internal, "\xff"}, out));

    EXPECT(decodeClient(hex("01 0001 0000 0018 0000000000000000 0000 "
                            "00000000"))
               .error == DecodeError::OutOfRange);
    EXPECT(decodeClient(hex("03 0050 03e9 0000 0000")).error ==
           DecodeError::OutOfRange);
    EXPECT(decodeClient(hex("02")).error == DecodeError::OutOfRange);
    EXPECT(decodeServer(hex("82 0000000000000000")).error ==
           DecodeError::OutOfRange);
    EXPECT(decodeServer(hex("85 000b 0000")).error == DecodeError::OutOfRange);
    EXPECT(decodeServer(hex("85 0000 0000")).error == DecodeError::OutOfRange);
}

TEST_CASE("Malformed framing is classified") {
    EXPECT(decodeClient("").error == DecodeError::Truncated);
    EXPECT(decodeServer("").error == DecodeError::Truncated);
    EXPECT(decodeClient(hex("04 0000")).error == DecodeError::Truncated);
    EXPECT(decodeClient(hex("04 0000000000000001 00")).error ==
           DecodeError::Trailing);
    EXPECT(decodeClient(hex("07 00")).error == DecodeError::Trailing);
    EXPECT(decodeClient(hex("06")).error == DecodeError::UnknownType);
    EXPECT(decodeClient(hex("00")).error == DecodeError::UnknownType);
    EXPECT(decodeClient(hex("42")).error == DecodeError::UnknownType);
    EXPECT(decodeClient(hex("82 00")).error == DecodeError::WrongDirection);
    EXPECT(decodeClient(hex("ff")).error == DecodeError::WrongDirection);
    EXPECT(decodeServer(hex("02 6c")).error == DecodeError::WrongDirection);
    EXPECT(decodeServer(hex("07")).error == DecodeError::WrongDirection);
    EXPECT(decodeServer(hex("06")).error == DecodeError::UnknownType);
    EXPECT(decodeServer(hex("80")).error == DecodeError::UnknownType);
    // A string that claims more bytes than the message holds.
    EXPECT(decodeServer(hex("81 0001 0005 6162 ")).error ==
           DecodeError::Truncated);
    // A session id longer than the limit is TooLarge, not accepted.
    std::string longId = hex("81 0001 0041") + std::string(65, 'a') +
                         hex("0000000000000000 00 00000000");
    EXPECT(decodeServer(longId).error == DecodeError::TooLarge);
}

TEST_CASE("Strings must be strictly valid UTF-8") {
    EXPECT(validUtf8(""));
    EXPECT(validUtf8("aåä€\xf0\x9f\x98\x80"));
    EXPECT(validUtf8("\xf4\x8f\xbf\xbf"));
    EXPECT_FALSE(validUtf8("\xc0\x80"));         // Overlong NUL.
    EXPECT_FALSE(validUtf8("\xe0\x80\x80"));     // Overlong.
    EXPECT_FALSE(validUtf8("\xed\xa0\x80"));     // Surrogate.
    EXPECT_FALSE(validUtf8("\xf4\x90\x80\x80")); // Above U+10FFFF.
    EXPECT_FALSE(validUtf8("\xc3"));             // Truncated.
    EXPECT_FALSE(validUtf8("\x80"));             // Stray continuation.
    EXPECT_FALSE(validUtf8("\xff"));
    EXPECT(decodeServer(hex("85 0001 0001 ff")).error ==
           DecodeError::BadString);
    EXPECT(decodeClient(hex("01 0001 0050 0018 0000000000000000 0001 ff "
                            "00000000"))
               .error == DecodeError::BadString);
}

TEST_CASE("Raw terminal bytes pass through Input and Output unchanged") {
    std::string all;
    for (int i = 0; i < 256; ++i) {
        all.push_back(char(i));
    }
    const auto input = decodeClient(encoded(Input{all}));
    EXPECT(input);
    EXPECT(as<Input>(*input.message).data == all);
    const auto output = decodeServer(encoded(Output{9, all}));
    EXPECT(output);
    EXPECT(as<Output>(*output.message).data == all);
}

TEST_CASE("Truncation at every length never decodes a valid prefix wrongly") {
    Hello hello;
    hello.sessionId = "session";
    const auto bytes = encoded(hello);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        EXPECT(!decodeClient(std::string_view(bytes).substr(0, length)));
    }
    const auto welcome = encoded(Welcome{1, "abc", 0, 0, 0});
    for (std::size_t length = 0; length < welcome.size(); ++length) {
        EXPECT(!decodeServer(std::string_view(welcome).substr(0, length)));
    }
}

TEST_CASE("Random bytes never crash and valid decodes re-encode identically") {
    std::mt19937 random(12345);
    int accepted = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string bytes(random() % 40, '\0');
        for (auto &byte : bytes) {
            byte = char(random());
        }
        if (!bytes.empty() && round % 2 == 0) {
            // Bias toward real message types so layouts are exercised.
            static const unsigned char types[] = {
                1, 2, 3, 4, 5, 7, 0x81, 0x82, 0x83, 0x84, 0x85};
            bytes[0] = char(types[random() % sizeof(types)]);
        }
        const auto client = decodeClient(bytes);
        if (client) {
            ++accepted;
            std::string out;
            EXPECT(encode(*client.message, out));
            EXPECT(out == bytes);
        }
        const auto server = decodeServer(bytes);
        if (server) {
            ++accepted;
            std::string out;
            EXPECT(encode(*server.message, out));
            EXPECT(out == bytes);
        }
    }
    EXPECT(accepted > 100); // The generator really reaches valid messages.
}

TEST_SUIT_END
