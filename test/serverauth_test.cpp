#include "auth.h"
#include "mls-unit-test/unittest.h"
#include <sys/stat.h>
#include <unistd.h>
using namespace server;
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
TEST_SUIT_END
