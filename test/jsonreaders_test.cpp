// What we ask of a strict JSON reader, once, run against the three readers of
// the tree (docs/tasks/json-readers.md):
//   jsonvalue   src/frontend/jsonvalue.h (settings.json)
//   snapshot    the Reader inside src/frontend/sessionsnapshot.h (reached
//   through a throw-away `#define private public`) fast-json   lib/fast-json
//   (the server's config), through a throw-away adapter; the submodule is not
//   touched
// Every case runs in a child process (a crash or a hang is a result, not the
// end of the run). The table of results is printed; the test fails only if
// jsonvalue.h, the reader we own and use for the settings, does not pass
// everything. `snapshot` has no writer and reads integers only, so the cases
// about writing back and number text are "n/a" or fail for it by design.
#include "jsonvalue.h"
#include "mls-unit-test/unittest.h"

#include <csignal>
#include <cstdio>
#include <fast-json/json.h>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define private public
#include "sessionsnapshot.h"
#undef private

namespace {

// The small interface each reader is adapted to.
struct Reader {
    virtual ~Reader() = default;
    virtual const char *name() const = 0;
    virtual bool parse(
        const std::string
            &text) = 0; // True when the text is accepted as one document.
    virtual bool canWrite() const = 0;
    virtual std::string write() = 0; // The parsed document written out again
                                     // (any layout).
    virtual std::optional<std::string> stringAt(
        const std::string &key) = 0;   // A top-level string member, decoded.
    virtual std::size_t members() = 0; // Top-level members.
    virtual std::vector<std::string> keys() = 0; // Top-level keys in document
                                                 // order.
};

struct ValueReader final : Reader {
    std::optional<frontend::json::Value> doc;
    const char *name() const override {
        return "jsonvalue";
    }
    bool parse(const std::string &text) override {
        doc = frontend::json::parse(text);
        return doc.has_value();
    }
    bool canWrite() const override {
        return true;
    }
    std::string write() override {
        return frontend::json::dump(*doc);
    }
    std::optional<std::string> stringAt(const std::string &key) override {
        const auto *v = doc->find(key);
        return v ? v->string() : std::nullopt;
    }
    std::size_t members() override {
        return doc->members().size();
    }
    std::vector<std::string> keys() override {
        std::vector<std::string> out;
        for (const auto &m : doc->members()) {
            out.push_back(m.first);
        }
        return out;
    }
};

struct SnapshotReader final : Reader {
    using Value = frontend::SessionSnapshot::Value;
    std::optional<Value> root;
    const char *name() const override {
        return "snapshot";
    }
    bool parse(const std::string &text) override {
        frontend::SessionSnapshot::Reader r{text};
        auto value = r.value(0);
        r.space();
        if (!value || r.pos != text.size() ||
            !std::holds_alternative<frontend::SessionSnapshot::Object>(
                value->v)) {
            return false;
        }
        root = std::move(value);
        return true;
    }
    bool canWrite() const override {
        return false;
    } // Read only.
    std::string write() override {
        return {};
    }
    const frontend::SessionSnapshot::Object &object() const {
        return std::get<frontend::SessionSnapshot::Object>(root->v);
    }
    std::optional<std::string> stringAt(const std::string &key) override {
        for (const auto &m : object()) {
            if (m.first == key &&
                std::holds_alternative<std::string>(m.second.v)) {
                return std::get<std::string>(m.second.v);
            }
        }
        return std::nullopt;
    }
    std::size_t members() override {
        return object().size();
    }
    std::vector<std::string> keys() override {
        std::vector<std::string> out;
        for (const auto &m : object()) {
            out.push_back(m.first);
        }
        return out;
    }
};

struct FastReader final : Reader {
    std::string held; // fast-json's nodes are views into the text: it must
                      // outlive the document.
    std::unique_ptr<json::JsonRoot> root;
    const char *name() const override {
        return "fast-json";
    }
    bool parse(const std::string &text) override {
        try {
            held = text;
            root = std::make_unique<json::JsonRoot>(json::parse_json(held));
            return !root->nodes.empty();
        }
        catch (const std::exception &) {
            return false;
        }
    }
    bool canWrite() const override {
        return true;
    }
    std::string write() override { // fast-json's DOM dump (its real writer,
                                   // JsonOut, does not take a parsed document).
        std::ostringstream out;
        json::dump(**root, out);
        return out.str();
    }
    std::optional<std::string> stringAt(const std::string &key) override {
        try {
            const auto *node = (*root)->find(key);
            return node ? std::optional<std::string>(node->str())
                        : std::nullopt;
        }
        catch (const std::exception &) {
            return std::nullopt;
        }
    }
    std::size_t members() override {
        std::size_t n = 0;
        for (auto it = (*root)->begin(); it != (*root)->end(); ++it) {
            ++n;
        }
        return n;
    }
    std::vector<std::string> keys() override {
        std::vector<std::string> out;
        for (auto it = (*root)->begin(); it != (*root)->end(); ++it) {
            out.push_back(std::string(it->value().value));
        }
        return out;
    }
};

enum class Result { Pass, Fail, NotApplicable, Crash, Timeout };

struct Case {
    const char *id;
    const char *what;
    std::function<Result(Reader &)> run;
};

bool accepts(Reader &r, const std::string &text) {
    return r.parse(text);
}
Result expect(bool ok) {
    return ok ? Result::Pass : Result::Fail;
}
// Every one of `texts` is refused.
Result refuses(Reader &r, const std::vector<std::string> &texts) {
    for (const auto &t : texts) {
        if (r.parse(t)) {
            return Result::Fail;
        }
    }
    return Result::Pass;
}
Result acceptsAll(Reader &r, const std::vector<std::string> &texts) {
    for (const auto &t : texts) {
        if (!r.parse(t)) {
            return Result::Fail;
        }
    }
    return Result::Pass;
}
std::string nest(int depth) {
    return "{\"a\":" + std::string(std::size_t(depth), '[') +
           std::string(std::size_t(depth), ']') + "}";
}

const std::vector<Case> &cases() {
    static const std::vector<Case> all = {
        {"R01",
         "Valid documents are accepted (objects, arrays, scalars inside, "
         "whitespace)",
         [](Reader &r) {
             return acceptsAll(r,
                               {"{}",
                                " \n\t{ } ",
                                R"({"a":1})",
                                R"({"a":[1,2,{"b":null}],"c":true,"d":false})",
                                R"({"s":"text","n":-12,"z":0})",
                                "{\"a\" :\r\n 1 }"});
         }},
        {"R02",
         "Broken syntax is refused (truncated, missing colon or comma, wrong "
         "quotes, bare words)",
         [](Reader &r) {
             return refuses(r,
                            {"{",
                             "{\"a\":",
                             "{\"a\"}",
                             "{\"a\" 1}",
                             "{\"a\":1 \"b\":2}",
                             "{'a':1}",
                             "{a:1}",
                             "{\"a\":nul}",
                             "{\"a\":tru}",
                             "[1 2]",
                             "{\"a\":[1,}",
                             "{\"a\":1,,\"b\":2}",
                             "{\"a\":\"x}",
                             "{\"a\":}",
                             "}{"});
         }},
        {"R03",
         "Trailing commas are refused",
         [](Reader &r) {
             return refuses(r,
                            {"{\"a\":1,}", "{\"a\":[1,2,]}", "{\"a\":[,1]}"});
         }},
        {"R04",
         "A duplicate key is refused",
         [](Reader &r) {
             return refuses(r, {R"({"a":1,"a":2})", R"({"x":{"b":1,"b":1}})"});
         }},
        {"R05",
         "Empty input and whitespace only are refused",
         [](Reader &r) {
             return refuses(r, {"", " ", "\n\t "});
         }},
        {"R06",
         "Text after the document is refused",
         [](Reader &r) {
             return refuses(r, {"{} x", "{}{}", "{}\n,", "{} 1"});
         }},
        {"R07",
         "Bad numbers are refused (leading zero, +, bare dot, trailing dot, "
         "NaN, Infinity, hex)",
         [](Reader &r) {
             return refuses(r,
                            {"{\"a\":01}",
                             "{\"a\":+1}",
                             "{\"a\":.5}",
                             "{\"a\":1.}",
                             "{\"a\":NaN}",
                             "{\"a\":Infinity}",
                             "{\"a\":0x10}",
                             "{\"a\":-}",
                             "{\"a\":1e}",
                             "{\"a\":1e+}",
                             "{\"a\":--1}"});
         }},
        {"R08",
         "Comments are refused",
         [](Reader &r) {
             return refuses(r,
                            {"// c\n{}",
                             "{/* c */}",
                             "{\"a\":1 // c\n}",
                             "{\"a\":1}/* */"});
         }},
        {"R09",
         "Bytes that are not UTF-8 in a string are refused (stray byte, "
         "overlong, truncated, surrogate)",
         [](Reader &r) {
             return refuses(r,
                            {std::string("{\"a\":\"\xff\"}"),
                             std::string("{\"a\":\"\xc0\xaf\"}"),
                             std::string("{\"a\":\"\xe2\x82\"}"),
                             std::string("{\"a\":\"\xed\xa0\x80\"}"),
                             std::string("{\"a\":\"\xf5\x80\x80\x80\"}")});
         }},
        {"R10",
         "Valid UTF-8 is accepted and comes back as it was",
         [](Reader &r) {
             if (!accepts(
                     r,
                     std::string(
                         "{\"a\":\"\xc3\xa5\xe2\x82\xac\xf0\x9f\x94\xa5\"}"))) {
                 return Result::Fail;
             }
             return expect(r.stringAt("a") ==
                           std::string("\xc3\xa5\xe2\x82\xac\xf0\x9f\x94\xa5"));
         }},
        {"R11",
         "A lone surrogate escape is refused",
         [](Reader &r) {
             return refuses(r,
                            {R"({"a":"\ud800"})",
                             R"({"a":"\udc00"})",
                             R"({"a":"\ud800x"})",
                             R"({"a":"\ud800A"})"});
         }},
        {"R12",
         "A surrogate pair escape is accepted and decoded to one character",
         [](Reader &r) {
             if (!accepts(r, R"({"a":"\ud83d\udd25"})")) {
                 return Result::Fail;
             }
             return expect(r.stringAt("a") == std::string("\xf0\x9f\x94\xa5"));
         }},
        {"R13",
         "The escapes are decoded (quote, backslash, slash, b f n r t, "
         "\\u00e5)",
         [](Reader &r) {
             if (!accepts(r, R"({"a":"q\"b\\c\/d\b\f\n\r\t\u00e5"})")) {
                 return Result::Fail;
             }
             return expect(r.stringAt("a") ==
                           std::string("q\"b\\c/d\b\f\n\r\t\xc3\xa5"));
         }},
        {"R14",
         "Unknown escapes and bad \\u escapes are refused",
         [](Reader &r) {
             return refuses(r,
                            {R"({"a":"\q"})",
                             R"({"a":"\u12"})",
                             R"({"a":"\uZZZZ"})",
                             R"({"a":"x\)"});
         }},
        {"R15",
         "A raw control character in a string is refused (tab, newline, 0x01)",
         [](Reader &r) {
             return refuses(r,
                            {std::string("{\"a\":\"x\ty\"}"),
                             std::string("{\"a\":\"x\ny\"}"),
                             std::string("{\"a\":\"\x01\"}")});
         }},
        {"R16",
         "A NUL byte in the text is refused",
         [](Reader &r) {
             return refuses(r,
                            {std::string("{\"a\":\"x\0y\"}", 11),
                             std::string("{\0}", 3),
                             std::string("{}\0", 3)});
         }},
        {"R17",
         "Nesting 20 deep is accepted",
         [](Reader &r) { return expect(accepts(r, nest(20))); }},
        {"R18",
         "Very deep nesting (5000) is refused without crashing",
         [](Reader &r) { return refuses(r, {nest(5000)}); }},
        {"R19",
         "A document over 2 MiB is refused",
         [](Reader &r) {
             return refuses(r,
                            {"{\"a\":\"" +
                             std::string(2u * 1024 * 1024 + 16, 'x') + "\"}"});
         }},
        {"R20",
         "A document of a few hundred KiB with many members is accepted",
         [](Reader &r) {
             std::string text = "{";
             for (int i = 0; i < 20000; ++i) {
                 text += (i ? "," : "") + std::string("\"key") +
                         std::to_string(i) + "\":\"value\"";
             }
             text += "}";
             return expect(accepts(r, text) && r.members() == 20000);
         }},
        {"R21",
         "The order of the members is kept",
         [](Reader &r) {
             if (!accepts(r, R"({"z":1,"a":2,"m":3,"b":4})")) {
                 return Result::Fail;
             }
             return expect(r.keys() ==
                           std::vector<std::string>({"z", "a", "m", "b"}));
         }},
        {"R22",
         "The document can be written back (the reader has a writer)",
         [](Reader &r) {
             if (!r.canWrite()) {
                 return Result::NotApplicable;
             }
             if (!accepts(r, R"({"a":1,"b":[true,null],"c":{"d":"x"}})")) {
                 return Result::Fail;
             }
             return expect(!r.write().empty());
         }},
        {"R23",
         "Writing back and reading again gives the same document (members, "
         "order)",
         [](Reader &r) {
             if (!r.canWrite()) {
                 return Result::NotApplicable;
             }
             if (!accepts(r, R"({"z":1,"a":[1,2,3],"m":{"k":"v"}})")) {
                 return Result::Fail;
             }
             const auto out = r.write();
             if (!r.parse(out)) {
                 return Result::Fail;
             }
             return expect(r.keys() ==
                           std::vector<std::string>({"z", "a", "m"}));
         }},
        {"R24",
         "Members this program does not know survive the round trip",
         [](Reader &r) {
             if (!r.canWrite()) {
                 return Result::NotApplicable;
             }
             if (!accepts(
                     r,
                     R"({"known":"x","future":{"deep":[1,2,{"x":null}]},"note":"keep me"})")) {
                 return Result::Fail;
             }
             const auto out = r.write();
             return expect(out.find("future") != std::string::npos &&
                           out.find("deep") != std::string::npos &&
                           out.find("keep me") != std::string::npos);
         }},
        {"R25",
         "Numbers keep their text (1.0, 1e3, -0, 1.50, a 20 digit integer)",
         [](Reader &r) {
             if (!r.canWrite()) {
                 return Result::NotApplicable;
             }
             if (!accepts(
                     r,
                     R"({"a":1.0,"b":1e3,"c":-0,"d":1.50,"e":12345678901234567890,"f":-1.5E-3})")) {
                 return Result::Fail;
             }
             const auto out = r.write();
             for (const char *text : {"1.0",
                                      "1e3",
                                      "-0",
                                      "1.50",
                                      "12345678901234567890",
                                      "-1.5E-3"}) {
                 if (out.find(text) == std::string::npos) {
                     return Result::Fail;
                 }
             }
             return Result::Pass;
         }},
        {"R26",
         "Floats and big integers are accepted as documents (they are only not "
         "integers)",
         [](Reader &r) {
             return acceptsAll(r,
                               {R"({"a":1.5})",
                                R"({"a":1e100})",
                                R"({"a":12345678901234567890})"});
         }},
        {"R27",
         "Strings with quotes, backslashes, control characters and UTF-8 round "
         "trip through the writer",
         [](Reader &r) {
             if (!r.canWrite()) {
                 return Result::NotApplicable;
             }
             const std::string source =
                 std::string("{\"a\":\"q\\\"b\\\\c\\n\\t\\u0001 \xc3\xa5\"}");
             if (!accepts(r, source)) {
                 return Result::Fail;
             }
             const auto before = r.stringAt("a");
             if (!r.parse(r.write())) {
                 return Result::Fail;
             }
             return expect(before.has_value() && r.stringAt("a") == before);
         }},
        {"R28",
         "A key and a string value with escapes are read the same way",
         [](Reader &r) {
             if (!accepts(r, R"({"k\n":"v\n"})")) {
                 return Result::Fail;
             }
             return expect(r.stringAt("k\n") == std::string("v\n"));
         }},
    };
    return all;
}

Result runInChild(const Case &c, int reader) {
    int fds[2];
    if (pipe(fds) != 0) {
        return Result::Crash;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        std::unique_ptr<Reader> r =
            reader == 0   ? std::unique_ptr<Reader>(new ValueReader)
            : reader == 1 ? std::unique_ptr<Reader>(new SnapshotReader)
                          : std::unique_ptr<Reader>(new FastReader);
        Result result = Result::Crash;
        try {
            result = c.run(*r);
        }
        catch (...) {
            result = Result::Crash;
        }
        const char byte = char(result);
        (void)!write(fds[1], &byte, 1);
        _exit(0);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char byte = 0;
    const bool got = read(fds[0], &byte, 1) == 1;
    close(fds[0]);
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status) == SIGALRM ? Result::Timeout : Result::Crash;
    }
    return got ? Result(byte) : Result::Crash;
}

const char *mark(Result r) {
    switch (r) {
    case Result::Pass:
        return "pass";
    case Result::Fail:
        return "FAIL";
    case Result::NotApplicable:
        return "n/a";
    case Result::Crash:
        return "CRASH";
    case Result::Timeout:
        return "HANG";
    }
    return "?";
}
} // namespace

TEST_SUIT_BEGIN(JsonReaders)

TEST_CASE(
    "The requirements, against the three readers (the table is printed)") {
    const char *names[3] = {"jsonvalue", "snapshot", "fast-json"};
    int passes[3] = {0, 0, 0}, fails[3] = {0, 0, 0}, na[3] = {0, 0, 0};
    std::printf("\n%-5s %-10s %-10s %-10s  %s\n",
                "case",
                names[0],
                names[1],
                names[2],
                "requirement");
    bool ownedPasses = true;
    for (const auto &c : cases()) {
        Result results[3];
        for (int i = 0; i < 3; ++i) {
            results[i] = runInChild(c, i);
            (results[i] == Result::Pass            ? passes[i]
             : results[i] == Result::NotApplicable ? na[i]
                                                   : fails[i])++;
        }
        ownedPasses = ownedPasses && (results[0] == Result::Pass ||
                                      results[0] == Result::NotApplicable);
        std::printf("%-5s %-10s %-10s %-10s  %s\n",
                    c.id,
                    mark(results[0]),
                    mark(results[1]),
                    mark(results[2]),
                    c.what);
    }
    std::printf("%-5s %-10s %-10s %-10s\n",
                "pass",
                (std::to_string(passes[0]) + "/" +
                 std::to_string(cases().size() - std::size_t(na[0])))
                    .c_str(),
                (std::to_string(passes[1]) + "/" +
                 std::to_string(cases().size() - std::size_t(na[1])))
                    .c_str(),
                (std::to_string(passes[2]) + "/" +
                 std::to_string(cases().size() - std::size_t(na[2])))
                    .c_str());
    EXPECT(ownedPasses); // jsonvalue.h is the one we maintain: it must meet
                         // every requirement.
}

TEST_SUIT_END
