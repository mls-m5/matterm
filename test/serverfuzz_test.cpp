#include "auth.h"
#include "config.h"
#include "message.h"
#include "mls-unit-test/unittest.h"

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

namespace {
constexpr unsigned seed = 0x5eed7103;
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
std::string mutate(std::mt19937 &random, std::string bytes) {
    switch (random() % 4) {
    case 0:
        if (!bytes.empty())
            bytes[random() % bytes.size()] = static_cast<char>(random());
        break;
    case 1:
        bytes.resize(random() % (bytes.size() + 1));
        break;
    case 2:
        bytes.insert(
            random() % (bytes.size() + 1), 1, static_cast<char>(random()));
        break;
    case 3:
        bytes += static_cast<char>(random());
        break;
    }
    return bytes;
}
std::string quoteJson(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : bytes) {
        const auto shortAt =
            std::string_view("\b\f\n\r\t").find(static_cast<char>(c));
        if (shortAt != std::string_view::npos) {
            result += '\\';
            result += std::string_view("bfnrt")[shortAt];
        }
        else if (c < 32) {
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        }
        else {
            if (c == '"' || c == '\\')
                result += '\\';
            result += c;
        }
    }
    return result + '"';
}
bool acceptsConfig(std::string_view text) {
    server::Settings settings;
    try {
        server::applyConfigText(text, settings, "fuzz");
        return true;
    }
    catch (const server::ConfigError &) {
        return false;
    }
}
} // namespace
TEST_SUIT_BEGIN(ServerFuzz)
TEST_CASE("Password mutations terminate and accepted results round-trip") {
    std::mt19937 random(seed);
    const std::vector<std::string> corpus{
        "{\"password\":\"long correct password for tests\"}",
        "{\"password\":\"\\ud83d\\ude00\"}",
        "{\"password\":\"\\u0000\\n\\t\\\\\\\"\"}",
        "{\"password\":\"\xc3\xa5\"}"};
    for (unsigned trial = 0; trial < 2000; ++trial) {
        auto bytes = mutate(random, corpus[random() % corpus.size()]);
        if (trial % 5 == 0) {
            bytes.resize(random() % 1024);
            for (auto &c : bytes)
                c = static_cast<char>(random());
        }
        const auto password = server::parsePassword(bytes);
        if (password) {
            EXPECT(check(bytes.size() <= 512 && password->size() <= 256 &&
                             protocol::validUtf8(*password),
                         trial,
                         bytes));
            EXPECT(check(server::parsePassword(
                             "{\"password\":" + quoteJson(*password) + "}") ==
                             password,
                         trial,
                         bytes));
            EXPECT(check(!server::parsePassword(bytes + "x"), trial, bytes));
        }
    }
}
TEST_CASE("Password round-trip respects the encoded body limit") {
    std::string bytes = "{\"password\":\"";
    for (unsigned i = 0; i < 100; ++i)
        bytes += "\\n";
    bytes += "\"}";
    const auto password = server::parsePassword(bytes);
    EXPECT(check(password && password->size() == 100, 0, bytes));
    EXPECT(check(server::parsePassword("{\"password\":" + quoteJson(*password) +
                                       "}") == password,
                 0,
                 bytes));
}
TEST_CASE("Password JSON rejects raw controls, invalid UTF-8, bad escapes and "
          "duplicate fields") {
    const std::vector<std::string> invalid{std::string(1, '\xff'),
                                           "\xc0\xaf",
                                           "\xed\xa0\x80",
                                           "\xf4\x90\x80\x80",
                                           "\xe2\x82",
                                           "\\ud800",
                                           "\\udc00",
                                           "\\uZZZZ",
                                           "\\q",
                                           "\n",
                                           std::string(1, '\0')};
    for (unsigned trial = 0; trial < invalid.size(); ++trial) {
        const auto bytes = "{\"password\":\"" + invalid[trial] + "\"}";
        EXPECT(check(!server::parsePassword(bytes), trial, bytes));
    }
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 200; ++trial) {
        const auto value = std::to_string(random());
        const auto bytes =
            "{\"password\":\"" + value + "\",\"password\":\"other\"}";
        EXPECT(check(!server::parsePassword(bytes), trial, bytes));
    }
}
TEST_CASE("Cookie fuzz preserves exact values and refuses ambiguous duplicate "
          "session fields") {
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 1200; ++trial) {
        const std::string token(64, "0123456789abcdef"[random() % 16]);
        auto bytes = "other=ignored; matterm_session=" + token + "; last=value";
        EXPECT(check(server::sessionCookie(bytes) ==
                         std::optional<std::string>(token),
                     trial,
                     bytes));
        bytes += "; matterm_session=" + token;
        EXPECT(check(!server::sessionCookie(bytes), trial, bytes));
        bytes = mutate(random, bytes);
        const auto parsed = server::sessionCookie(bytes);
        if (parsed) {
            EXPECT(check(parsed->size() <= bytes.size() &&
                             bytes.find("matterm_session=" + *parsed) !=
                                 std::string::npos &&
                             parsed->find(';') == std::string::npos,
                         trial,
                         bytes));
        }
    }
}
TEST_CASE(
    "Config mutations preserve transactional state and canonical round-trip") {
    std::mt19937 random(seed);
    const std::vector<std::string> corpus{
        "{}",
        "{\"port\":8081}",
        "{\"process\":{\"arguments\":[\"/bin/sh\",\"-i\"]}}",
        "{\"_note\":[null,true,-1,1.5,1e3,{\"nested\":[]}]}"};
    for (unsigned trial = 0; trial < 1000; ++trial) {
        auto bytes = corpus[random() % corpus.size()];
        if (trial % 3)
            bytes = mutate(random, std::move(bytes));
        if (trial % 7 == 0) {
            bytes.resize(random() % 256);
            for (auto &c : bytes)
                c = static_cast<char>(random());
        }
        server::Settings settings;
        const auto before = server::printConfig(settings);
        try {
            server::applyConfigText(bytes, settings, "fuzz");
            const auto canonical = server::printConfig(settings);
            server::Settings restored;
            server::applyConfigText(canonical, restored, "canonical");
            EXPECT(check(
                server::printConfig(restored) == canonical, trial, bytes));
        }
        catch (const server::ConfigError &) {
            EXPECT(
                check(server::printConfig(settings) == before, trial, bytes));
        }
    }
}
TEST_CASE("Config rejects malformed strings even in ignored comments and "
          "enforces integer policy") {
    const std::vector<std::string> invalidStrings{std::string(1, '\xff'),
                                                  "\xc0\xaf",
                                                  "\xed\xa0\x80",
                                                  "\xe2\x82",
                                                  "\n",
                                                  std::string(1, '\0'),
                                                  "\\q",
                                                  "\\ud800",
                                                  "\\udc00",
                                                  "\\uZZZZ",
                                                  "\\u0000"};
    for (unsigned trial = 0; trial < invalidStrings.size(); ++trial) {
        for (const auto &key : {"credentials", "_note"}) {
            const auto bytes = "{\"" + std::string(key) + "\":\"" +
                               invalidStrings[trial] + "\"}";
            EXPECT(check(!acceptsConfig(bytes), trial, bytes));
        }
    }
    for (const auto &value : {"-1",
                              "1.5",
                              "1e3",
                              "65536",
                              "true",
                              "null",
                              "\"80\"",
                              "[80]",
                              "01",
                              "1e",
                              "+1"}) {
        const auto bytes = "{\"port\":" + std::string(value) + "}";
        EXPECT(check(!acceptsConfig(bytes), 0, bytes));
    }
    for (const auto &bytes : {"{\"port\":80,\"port\":81}",
                              "{\"port\":80 \"port\":81}",
                              "{\"port\":80}x",
                              "{\"port\":80",
                              "{\"unknown\":0}"})
        EXPECT(check(!acceptsConfig(bytes), 0, bytes));
    const auto deep = "{\"_note\":" + std::string(500, '[') + "0" +
                      std::string(500, ']') + "}";
    EXPECT(check(acceptsConfig(deep), 0, deep));
}
TEST_SUIT_END
