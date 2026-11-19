#include "auth.h"
#include "mls-unit-test/unittest.h"
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>
using namespace server;
namespace {
// A string literal with embedded NULs, sized from the literal itself.
template <std::size_t N>
std::string bytes(const char (&literal)[N]) {
    return std::string(literal, N - 1);
}
} // namespace
TEST_SUIT_BEGIN(ServerAuth)
TEST_CASE("Argon2id verifies only the configured password") {
    const std::string password = "a long random test password 12345";
    PasswordVerifier verifier(hashPassword(password));
    EXPECT(verifier.verify(password));
    EXPECT(!verifier.verify("incorrect"));
    PasswordVerifier dummy;
    EXPECT(!dummy.verify("dummy password not used for authentication"));
    bool rejected = false;
    try {
        hashPassword("short");
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    EXPECT(rejected);
}
TEST_CASE(
    "Only digested issued tokens validate; expiry, rotation and single claim") {
    Clock::time_point now{};
    SessionStore store({std::chrono::seconds(10), std::chrono::seconds(20)},
                       [&] { return now; });
    auto token = store.issue();
    EXPECT_EQ(token.size(), std::size_t(64));
    EXPECT(store.valid(token));
    EXPECT(!store.valid(std::string(64, '0')));
    EXPECT(!store.valid("forged"));
    EXPECT(store.claim(token));
    EXPECT(!store.claim(token));
    now += std::chrono::seconds(10);
    EXPECT(!store.valid(token));
    token = store.issue();
    now += std::chrono::seconds(9);
    EXPECT(store.valid(token));
    now += std::chrono::seconds(9);
    EXPECT(store.valid(token));
    now += std::chrono::seconds(2);
    EXPECT(!store.valid(token));
    token = store.issue();
    const auto replacement = store.issue();
    EXPECT(!store.valid(token));
    EXPECT(store.valid(replacement));
    store.revoke();
    EXPECT(!store.valid(replacement));
}
TEST_CASE("JSON password field and strict cookie parsing") {
    EXPECT(parsePassword("{\"password\":\"abc\"}") ==
           std::optional<std::string>("abc"));
    EXPECT(
        parsePassword(" { \"password\" : \"a\\u0021\\n\\ud83d\\ude00\" } ") ==
        std::optional<std::string>("a!\n\xf0\x9f\x98\x80"));
    for (const auto input : {"{}",
                             "{\"password\":1}",
                             "{\"password\":\"x\",\"password\":\"y\"}",
                             "{\"password\":\"x\"}x",
                             "{\"password\":\"\\ud800\"}"}) {
        EXPECT(!parsePassword(input));
    }
    EXPECT(sessionCookie("other=x; matterm_session=abc") ==
           std::optional<std::string>("abc"));
    EXPECT(!sessionCookie("matterm_session=a; matterm_session=b"));
    EXPECT(!sessionCookie("other=x"));
}
TEST_CASE("Credential file is owner-only and reloadable") {
    char directory[] = "/tmp/matterm-auth-XXXXXX";
    EXPECT(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/credentials";
    savePassword(path, "a long random test password 12345");
    struct stat st {};
    EXPECT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ((st.st_mode & 0777), mode_t(0600));
    EXPECT(PasswordVerifier(loadPasswordHash(path))
               .verify("a long random test password 12345"));
    ::unlink(path.c_str());
    ::rmdir(directory);
}
TEST_CASE("Password JSON: escapes decode to exact UTF-8") {
    const auto parse = [](const char *json) { return parsePassword(json); };
    EXPECT(parse("{\"password\":\"\\u00e9\\u00E9\"}") ==
           std::optional<std::string>("\xc3\xa9\xc3\xa9"));
    EXPECT(parse("{\"password\":\"\\u20ac\"}") ==
           std::optional<std::string>("\xe2\x82\xac"));
    EXPECT(parse("{\"password\":\"\\uffff\"}") ==
           std::optional<std::string>("\xef\xbf\xbf"));
    EXPECT(parse("{\"password\":\"\\udbff\\udfff\"}") ==
           std::optional<std::string>("\xf4\x8f\xbf\xbf"));
    EXPECT(parse("{\"password\":\"\\ud800\\udc00\"}") ==
           std::optional<std::string>("\xf0\x90\x80\x80"));
    EXPECT(parse("{\"password\":\"\\/\\\\\\\"\\b\\f\\r\\t\"}") ==
           std::optional<std::string>("/\\\"\b\f\r\t"));
}
TEST_CASE("Password JSON: surrogates, bad escapes and control bytes") {
    for (const char *input : {
             "{\"password\":\"\\udc00\"}",        // Lone low surrogate.
             "{\"password\":\"\\ud800\"}",        // Lone high surrogate.
             "{\"password\":\"\\ud800x\"}",       // High, then a plain char.
             "{\"password\":\"\\ud800\\u0041\"}", // High, then not low.
             "{\"password\":\"\\ud800\\ud800\"}", // High, then high.
             "{\"password\":\"\\udc00\\ud800\"}", // Reversed pair.
             "{\"password\":\"\\ud800\\u\"}",     // Truncated low.
             "{\"password\":\"\\u12\"}",          // Short escape.
             "{\"password\":\"\\u12g4\"}",        // Non-hex digit.
             "{\"password\":\"\\x41\"}",          // Unknown escape.
             "{\"password\":\"\\\"}",             // Escape at the end.
             "{\"password\":\"abc}",              // Unterminated.
             "{\"password\":\"a\tb\"}",           // Raw TAB.
             "{\"password\":\"a\nb\"}",           // Raw LF.
             "{\"password\":\"a\x01"
             "b\"}", // Raw control byte.
         })
        EXPECT(!parsePassword(input));
}
TEST_CASE("Password JSON: NUL bytes") {
    // A raw NUL is rejected; an escaped one is data. Verification is
    // length-based, so a prefix of the password never authenticates.
    EXPECT(!parsePassword(bytes("{\"password\":\"a\0b\"}")));
    // The same document without the NUL is accepted: rejected for the NUL.
    EXPECT(parsePassword(bytes("{\"password\":\"ab\"}")).has_value());
    const auto parsed =
        parsePassword("{\"password\":\"a long random test pass\\u0000word\"}");
    EXPECT(parsed.has_value());
    const auto password = bytes("a long random test pass\0word");
    EXPECT(parsed == std::optional<std::string>(password));
    PasswordVerifier verifier(hashPassword(password));
    EXPECT(verifier.verify(*parsed));
    EXPECT(!verifier.verify("a long random test pass"));
    EXPECT(!verifier.verify(bytes("a long random test pass\0")));
    EXPECT(!parsePassword(bytes("{\"pass\0word\":\"x\"}")));
}
TEST_CASE("Password JSON: structure, trailing bytes and sizes") {
    EXPECT(parsePassword("{\"password\":\"x\"}\n\t ").has_value());
    for (const auto &input : {std::string(""),
                              std::string("{"),
                              std::string("{\"password\":\"x\""),
                              std::string("{\"password\":\"x\"}}"),
                              std::string("{\"password\":\"x\"},"),
                              std::string("{\"password\":\"x\",}"),
                              bytes("{\"password\":\"x\"}\0"),
                              std::string("\xef\xbb\xbf{\"password\":\"x\"}"),
                              std::string("[{\"password\":\"x\"}]"),
                              std::string("{\"Password\":\"x\"}"),
                              std::string("{\"password\":null}"),
                              std::string("{\"password\":{\"a\":\"x\"}}"),
                              std::string("{\"password\" \"x\"}"),
                              std::string("{password:\"x\"}"),
                              std::string("{'password':'x'}")})
        EXPECT(!parsePassword(input));
    // The key may use an escape; it still has to decode to exactly "password".
    EXPECT(parsePassword("{\"passw\\u006frd\":\"x\"}").has_value());
    const auto frame = [](std::size_t password, std::size_t padding) {
        return std::string(padding, ' ') + "{\"password\":\"" +
               std::string(password, 'p') + "\"}";
    };
    EXPECT(parsePassword(frame(256, 0)).has_value());
    EXPECT(!parsePassword(frame(257, 0)));
    EXPECT(frame(1, 512 - frame(1, 0).size()).size() == 512);
    EXPECT(parsePassword(frame(1, 512 - frame(1, 0).size())).has_value());
    EXPECT(!parsePassword(frame(1, 513 - frame(1, 0).size())));
    EXPECT(!parsePassword(std::string(100000, '{')));
}
TEST_CASE("Session cookie: duplicates, names, separators and size") {
    const std::string token(64, 'a');
    EXPECT(sessionCookie("matterm_session=" + token) ==
           std::optional<std::string>(token));
    EXPECT(sessionCookie("a=b; matterm_session=" + token + "; c=d") ==
           std::optional<std::string>(token));
    EXPECT(sessionCookie("a=b;matterm_session=" + token) ==
           std::optional<std::string>(token));
    EXPECT(sessionCookie("matterm_session=" + token + ";") ==
           std::optional<std::string>(token));
    EXPECT(sessionCookie("matterm_session=") == std::optional<std::string>(""));
    // Duplicates are refused, even when one of them is empty or differs.
    EXPECT(!sessionCookie("matterm_session=; matterm_session=" + token));
    EXPECT(!sessionCookie("matterm_session=" + token +
                          "; matterm_session=" + token));
    EXPECT(!sessionCookie("matterm_session=a;matterm_session=b"));
    // Other names, case and prefixes never match.
    EXPECT(!sessionCookie(""));
    EXPECT(!sessionCookie(";"));
    EXPECT(!sessionCookie("MATTERM_SESSION=" + token));
    EXPECT(!sessionCookie("xmatterm_session=" + token));
    EXPECT(!sessionCookie("matterm_session_x=" + token));
    EXPECT(!sessionCookie("matterm_session =" + token));
    EXPECT(!sessionCookie("matterm_session" + token));
    EXPECT(!sessionCookie("a=matterm_session=" + token));
    EXPECT(!sessionCookie("a=b;\tmatterm_session=" + token));
    // The value is whatever follows up to the separator; the store decides.
    EXPECT(sessionCookie("matterm_session=" + token + " ") ==
           std::optional<std::string>(token + " "));
    EXPECT(sessionCookie("matterm_session=\"" + token + "\"") ==
           std::optional<std::string>("\"" + token + "\""));
    const std::string huge(8000, 'z');
    EXPECT(sessionCookie("matterm_session=" + huge) ==
           std::optional<std::string>(huge));
}
TEST_CASE("Session store refuses cookie values that are not its tokens") {
    SessionStore store;
    const auto token = store.issue();
    for (const auto &candidate : {std::string(),
                                  token + " ",
                                  " " + token,
                                  "\"" + token + "\"",
                                  token.substr(1),
                                  token + "0",
                                  std::string(8000, 'z'),
                                  std::string(64, '\0'),
                                  std::string(64, 'A')})
        EXPECT(!store.valid(candidate) && !store.claim(candidate));
    // Uppercase hex is another string, not the same token.
    std::string upper = token;
    for (auto &c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper != token)
        EXPECT(!store.valid(upper));
    EXPECT(store.valid(token));
    EXPECT(store.claim(token));
}
TEST_SUIT_END
