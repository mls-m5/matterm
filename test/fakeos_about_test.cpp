#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

// The numbers `about` prints are counted from the repository when it is built;
// here they are counted again, independently, from the files.
namespace fs = std::filesystem;
using fakeos::Config;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

std::string slurp(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}
long newlines(const fs::path &path) {
    const auto text = slurp(path);
    return long(std::count(text.begin(), text.end(), '\n'));
}
bool hasExtension(const fs::path &path,
                  std::initializer_list<const char *> extensions) {
    for (const char *e : extensions) {
        if (path.extension() == e) {
            return true;
        }
    }
    return false;
}
// Files of a directory (recursively or not) with the given extensions.
std::vector<fs::path> files(const fs::path &dir,
                            std::initializer_list<const char *> extensions,
                            bool recursive) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) {
        return out;
    }
    auto take = [&](const fs::directory_entry &entry) {
        if (entry.is_regular_file() && hasExtension(entry.path(), extensions)) {
            out.push_back(entry.path());
        }
    };
    if (recursive) {
        for (const auto &entry : fs::recursive_directory_iterator(dir)) {
            take(entry);
        }
    }
    else {
        for (const auto &entry : fs::directory_iterator(dir)) {
            take(entry);
        }
    }
    return out;
}
struct Count {
    long files = 0, lines = 0;
};
Count count(const std::vector<fs::path> &paths) {
    Count c;
    for (const auto &p : paths) {
        ++c.files;
        c.lines += newlines(p);
    }
    return c;
}
void append(std::vector<fs::path> &to, const std::vector<fs::path> &from) {
    to.insert(to.end(), from.begin(), from.end());
}

struct Counted {
    Count cpp, web, tests, docs, demo;
    long cases = 0;
};
Counted recount() {
    const fs::path root = MATTERM_SOURCE_DIR;
    Counted r;
    std::vector<fs::path> cpp;
    for (const char *dir : {"src", "app", "demo", "bench"}) {
        append(cpp, files(root / dir, {".cpp", ".h"}, true));
    }
    append(cpp, files(root / "web", {".cpp", ".h"}, false));
    r.cpp = count(cpp);
    std::vector<fs::path> web =
        files(root / "web", {".js", ".html", ".css"}, false);
    append(web, files(root / "web" / "tools", {".js"}, false));
    r.web = count(web);
    const auto tests =
        files(root / "test", {".cpp", ".h", ".py", ".cmake"}, false);
    r.tests = count(tests);
    for (const auto &p : files(root / "test", {".cpp"}, false)) {
        const auto text = slurp(p);
        for (auto at = text.find("TEST_CASE("); at != std::string::npos;
             at = text.find("TEST_CASE(", at + 1)) {
            ++r.cases;
        }
    }
    std::vector<fs::path> docs = files(root / "docs", {".md"}, false);
    append(docs, files(root, {".md"}, false));
    r.docs = count(docs);
    std::vector<fs::path> demo =
        files(root / "src" / "fakeos", {".cpp", ".h"}, false);
    append(demo, files(root / "app" / "demo", {".cpp", ".h"}, false));
    r.demo = count(demo);
    return r;
}
} // namespace

TEST_SUIT_BEGIN(FakeosAbout)

TEST_CASE("about is a command and says what the project is made of") {
    Pc pc(false, 80, 60);
    pc.run("about");
    for (const char *fact : {"C++17",
                             "VT parser",
                             "terminal model",
                             "PTY",
                             "SDL2",
                             "WebAssembly",
                             "Emscripten",
                             "glyph atlas",
                             "matterm-tty",
                             "Boost.Beast",
                             "Argon2id",
                             "libsodium",
                             "Origin",
                             "Fuzz tests",
                             "benchmarks",
                             "security review",
                             "pinch",
                             "idle",
                             "without a server"}) {
        EXPECT(pc.shows(fact));
    }
    EXPECT(pc.last() == kPrompt);
    pc.run("man about");
    EXPECT(pc.shows("about - "));
}

TEST_CASE("It ends with the credit and the tools") {
    Pc pc(false, 80, 60);
    pc.run("about");
    EXPECT(pc.shows("Byggt av Mattias Lasersk"));
    EXPECT(pc.shows("Claude"));
    EXPECT(pc.shows("Pi coding agent"));
    EXPECT(pc.shows("OpenAI Codex"));
    // The credit comes last, then the prompt.
    std::size_t creditRow = 0, toolsRow = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        const auto row = pc.row(y);
        if (row.find("Byggt av Mattias Lasersk") != std::string::npos)
            creditRow = y;
        if (row.find("OpenAI Codex") != std::string::npos)
            toolsRow = y;
    }
    EXPECT(creditRow > 0 && toolsRow >= creditRow);
    EXPECT(toolsRow + 2 >= std::size_t(pc.model.cursor().row));
    // The name is spelled with its ö.
    bool umlaut = false;
    for (std::size_t x = 0; x < pc.model.columns(); ++x) {
        umlaut = umlaut || pc.model.cell(x, creditRow).codePoint == U'ö';
    }
    EXPECT(umlaut);
}

TEST_CASE("The numbers are counted from the repository, not made up") {
    const auto r = recount();
    EXPECT(r.cpp.lines > 1000 &&
           r.cases > 100); // The repository is really there.
    Pc pc(false, 80, 60);
    pc.run("about");
    EXPECT(pc.shows(std::to_string(r.cpp.lines) + " lines"));
    EXPECT(pc.shows(std::to_string(r.cpp.files) + " files"));
    EXPECT(pc.shows(std::to_string(r.cases) + " test cases"));
    EXPECT(pc.shows(std::to_string(r.tests.files) + " test files"));
    EXPECT(pc.shows(std::to_string(r.tests.lines) + " lines of tests"));
    EXPECT(pc.shows(std::to_string(r.web.lines) + " lines"));
    EXPECT(pc.shows(std::to_string(r.docs.files) + " documents"));
    EXPECT(pc.shows(std::to_string(r.docs.lines) + " lines of documents"));
    EXPECT(pc.shows(std::to_string(r.demo.lines) + " lines of this demo"));
}

TEST_CASE("The text is revealed with the animation when a pace is set") {
    Config config;
    config.intro = false;
    config.rows = 60;
    config.revealCharsPerSecond = 2000;
    Pc pc(config);
    pc.run("about");
    EXPECT(pc.sys.revealing());
    EXPECT(!pc.shows("Byggt av"));
    for (int i = 0; i < 400 && pc.sys.revealing(); ++i) {
        pc.advance(50);
    }
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.shows("Byggt av Mattias Lasersk"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Space shows the whole text at once") {
    Config config;
    config.intro = false;
    config.rows = 60;
    config.revealCharsPerSecond = 500;
    Pc pc(config);
    pc.run("about");
    pc.advance(300);
    EXPECT(pc.sys.revealing());
    pc.type(" ");
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.shows("OpenAI Codex"));
}

TEST_CASE("neofetch signs the picture") {
    Pc pc;
    pc.run("neofetch");
    bool signedBy = false, umlaut = false;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        if (pc.row(y).find("by Mattias Lasersk") != std::string::npos) {
            signedBy = true;
            for (std::size_t x = 0; x < pc.model.columns(); ++x) {
                umlaut = umlaut || pc.model.cell(x, y).codePoint == U'ö';
            }
        }
    }
    EXPECT(signedBy);
    EXPECT(umlaut);
    EXPECT(pc.shows("| |   MATTRIX    | |")); // The monitor is still there.
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
}

TEST_SUIT_END
