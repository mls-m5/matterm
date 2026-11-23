#include "jsonvalue.h"
#include "mls-unit-test/unittest.h"

using frontend::json::parse;
using frontend::json::Value;

TEST_SUIT_BEGIN(JsonValue)

TEST_CASE("Everything JSON has is read and written back the same (compact)") {
    const std::string text =
        R"({"a":1,"b":[true,false,null,"x\ny",-3.5e2],"c":{"d":"é","e":[]},"f":{}})";
    const auto value = parse(text);
    EXPECT(value.has_value());
    EXPECT_EQ(frontend::json::dump(*value), text);
}

TEST_CASE("Objects keep their key order, and lookups find keys") {
    const auto value = parse(R"({"z":1,"a":2,"m":3})");
    EXPECT(value.has_value());
    EXPECT(value->isObject());
    EXPECT_EQ(value->members().size(), std::size_t(3));
    EXPECT_EQ(value->members()[0].first, std::string("z"));
    EXPECT(value->find("a") != nullptr);
    EXPECT(value->find("q") == nullptr);
    EXPECT_EQ(value->find("a")->integer().value_or(-1), std::int64_t(2));
}

TEST_CASE("Scalars: strings, integers, booleans, the type checks") {
    const auto value = parse(
        R"({"s":"hi","n":42,"neg":-7,"t":true,"f":false,"z":null,"x":1.5})");
    EXPECT(value.has_value());
    EXPECT_EQ(value->find("s")->string().value_or(""), std::string("hi"));
    EXPECT_EQ(value->find("n")->integer().value_or(0), std::int64_t(42));
    EXPECT_EQ(value->find("neg")->integer().value_or(0), std::int64_t(-7));
    EXPECT(value->find("t")->boolean().value_or(false));
    EXPECT(!value->find("f")->boolean().value_or(true));
    EXPECT(value->find("z")->isNull());
    EXPECT(!value->find("x")
                ->integer()
                .has_value()); // 1.5 is a number but no integer.
    EXPECT(!value->find("s")->integer().has_value());
    EXPECT(!value->find("n")->string().has_value());
}

TEST_CASE("Broken documents are refused as a whole, with where") {
    for (const char *bad : {"",
                            "{",
                            "{\"a\":}",
                            "{\"a\":1,}",
                            "[1,2",
                            "{\"a\" 1}",
                            "{'a':1}",
                            "{\"a\":01}",
                            "{\"a\":1} x",
                            "{\"a\":1,\"a\":2}", // Duplicate key.
                            "{\"a\":\"\\q\"}",   // Unknown escape.
                            "{\"a\":\"\x01\"}",  // Control character.
                            "{\"a\":\"\xff\"}",  // Not UTF-8.
                            "{\"a\":NaN}",
                            "{\"a\":+1}",
                            "{\"a\":.5}",
                            "{\"a\":1.}",
                            "// comment\n{}",
                            "{\"a\":\"\\ud800\"}"}) {
        std::string error;
        EXPECT(!parse(bad, &error).has_value());
        EXPECT(!error.empty());
    }
}

TEST_CASE("Limits: depth and size") {
    std::string deep;
    for (int i = 0; i < 100; ++i) {
        deep += "[";
    }
    for (int i = 0; i < 100; ++i) {
        deep += "]";
    }
    EXPECT(!parse(deep).has_value());
    EXPECT(parse("[[[[1]]]]").has_value());
    EXPECT(!parse(std::string(2 << 20, ' ') + "{}").has_value());
}

TEST_CASE("Escapes and surrogate pairs") {
    const auto value =
        parse(R"({"s":"a\"b\\c\/d\b\f\n\r\t\u00e5\ud83d\udd25"})");
    EXPECT(value.has_value());
    EXPECT_EQ(value->find("s")->string().value_or(""),
              std::string("a\"b\\c/d\b\f\n\r\t\xc3\xa5\xf0\x9f\x94\xa5"));
}

TEST_CASE("Writing escapes what must be escaped and the output reads back") {
    Value object = Value::object();
    object.set("text",
               Value::text(std::string(
                   "quote\" back\\ nl\n tab\t ctl\x01 utf\xc3\xa5")));
    object.set("n", Value::number(-12));
    object.set("on", Value::boolean(true));
    const auto out = frontend::json::dump(object);
    const auto back = parse(out);
    EXPECT(back.has_value());
    EXPECT_EQ(back->find("text")->string().value_or(""),
              std::string("quote\" back\\ nl\n tab\t ctl\x01 utf\xc3\xa5"));
    EXPECT_EQ(back->find("n")->integer().value_or(0), std::int64_t(-12));
}

TEST_CASE(
    "Pretty output is the same document, indented, with a final newline") {
    const auto value = parse(R"({"a":1,"b":{"c":[1,2]},"d":[]})");
    const auto pretty = frontend::json::dump(*value, true);
    EXPECT(pretty.back() == '\n');
    EXPECT(pretty.find("\n  \"a\": 1") != std::string::npos);
    const auto again = parse(pretty);
    EXPECT(again.has_value());
    EXPECT_EQ(frontend::json::dump(*again), frontend::json::dump(*value));
}

TEST_CASE("set replaces a member in place and adds new ones at the end; erase "
          "removes") {
    auto value = *parse(R"({"a":1,"b":2})");
    value.set("a", Value::number(9));
    value.set("c", Value::number(3));
    EXPECT_EQ(frontend::json::dump(value),
              std::string(R"({"a":9,"b":2,"c":3})"));
    EXPECT(value.erase("b"));
    EXPECT(!value.erase("b"));
    EXPECT_EQ(frontend::json::dump(value), std::string(R"({"a":9,"c":3})"));
}

TEST_SUIT_END
