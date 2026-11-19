#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>
#include <vector>

// The playful files in the home directory: small, sweet and harmless.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

std::string fileText(const Pc &pc, const std::string &path) {
    const auto *node = pc.sys.world().fs.find(path);
    return node && !node->directory ? node->data : std::string("<missing>");
}
} // namespace

TEST_SUIT_BEGIN(FakeosFiles)

TEST_CASE("Downloads holds the files, .blush is hidden") {
    Pc pc(false, 100, 24);
    pc.run("ls");
    EXPECT(pc.shows("Downloads"));
    pc.run("clear");
    pc.run("ls Downloads");
    for (const char *name : {"definitely_not_homework.txt",
                             "private_DO_NOT_OPEN.txt",
                             "passwords_final_v3.txt",
                             "vacation_pics_(1).txt",
                             "totally_legit_movie.mkv.txt"}) {
        EXPECT(pc.shows(name));
    }
    EXPECT(!pc.shows(".blush"));
    pc.run("clear");
    pc.run("ls -a Downloads");
    EXPECT(pc.shows(".blush"));
    pc.run("cat Downloads/.blush");
    EXPECT(pc.shows("oh."));
}

TEST_CASE("The files say what they should") {
    Pc pc;
    const auto home = std::string("/home/guest/Downloads/");
    const auto homework = fileText(pc, home + "definitely_not_homework.txt");
    EXPECT(homework != "<missing>");
    EXPECT(homework.find("compiler") ==
           std::string::npos); // It is not about code.
    EXPECT(homework.find("grep") == std::string::npos);
    const auto secret = fileText(pc, home + "private_DO_NOT_OPEN.txt");
    const std::string end = "You opened it anyway. I knew you would.\n";
    EXPECT(secret.size() > end.size());
    EXPECT(secret.compare(secret.size() - end.size(), end.size(), end) == 0);
    const auto passwords = fileText(pc, home + "passwords_final_v3.txt");
    EXPECT(passwords.find("love") == 0);
    EXPECT(passwords.find("bad idea") != std::string::npos);
    const auto pictures = fileText(pc, home + "vacation_pics_(1).txt");
    EXPECT(pictures.find("<3") != std::string::npos);
    EXPECT(pictures.find("Trash") != std::string::npos); // The way on.
    const auto movie = fileText(pc, home + "totally_legit_movie.mkv.txt");
    EXPECT(movie.find("pipe") != std::string::npos);
    EXPECT(movie.find("process") != std::string::npos);
    EXPECT(fileText(pc, home + ".blush") == "oh.\n");
}

TEST_CASE("The files are small and short, and can be read from the prompt") {
    Pc pc(false, 100, 40);
    for (const char *name : {"definitely_not_homework.txt",
                             "private_DO_NOT_OPEN.txt",
                             "passwords_final_v3.txt",
                             "vacation_pics_(1).txt",
                             "totally_legit_movie.mkv.txt"}) {
        const auto text =
            fileText(pc, std::string("/home/guest/Downloads/") + name);
        EXPECT(text.size() > 5 && text.size() < 700);
    }
    pc.run("cd Downloads");
    pc.type("cat priv\t");
    EXPECT(pc.last().find("cat private_DO_NOT_OPEN.txt") != std::string::npos);
    pc.type("\r");
    EXPECT(pc.shows("You opened it anyway. I knew you would."));
    pc.run("clear");
    pc.run("cat vacation_pics_(1).txt");
    EXPECT(pc.shows("<3"));
    pc.run("clear");
    pc.run("cat 'totally_legit_movie.mkv.txt'");
    EXPECT(pc.shows("pipe"));
}

TEST_CASE("The romantic shell history and the feelings log") {
    Pc pc(false, 100, 40);
    pc.run("cat .bash_history_romantic");
    EXPECT(pc.shows("cd ~/heart"));
    EXPECT(pc.shows("sudo make love"));
    EXPECT(pc.shows("make: don't know how to make love. Stop."));
    EXPECT(pc.shows(".local/share/Trash/files/last_poem.txt"));
    pc.run("clear");
    pc.run("cat /var/log/feelings.log");
    EXPECT(pc.shows("WARN"));
    EXPECT(pc.shows("heart rate above threshold"));
    EXPECT(pc.shows("INFO"));
    EXPECT(pc.shows("crush detected on port 143"));
    pc.run("clear");
    pc.run("cat /etc/motd.love");
    EXPECT(pc.last() == kPrompt);
    EXPECT(fileText(pc, "/etc/motd.love").size() > 10);
}

TEST_CASE("make love fails like make would, also with sudo") {
    Pc pc;
    pc.run("make love");
    EXPECT(pc.shows("make: don't know how to make love. Stop."));
    pc.run("clear");
    pc.run("sudo make love");
    EXPECT(pc.shows("make: don't know how to make love. Stop."));
}

TEST_CASE("The history points to the Trash, where a last poem waits") {
    Pc pc(false, 100, 40);
    pc.run("ls -a .local/share/Trash/files");
    EXPECT(pc.shows("last_poem.txt"));
    pc.run("clear");
    pc.run("cat ~/.local/share/Trash/files/last_poem.txt");
    EXPECT(pc.shows("Exit code 0."));
    pc.run("clear");
    pc.run("find ~ -name 'last_poem*'");
    EXPECT(pc.shows("/home/guest/.local/share/Trash/files/last_poem.txt") ||
           pc.shows("last_poem.txt"));
    const auto poem =
        fileText(pc, "/home/guest/.local/share/Trash/files/last_poem.txt");
    EXPECT(poem.size() > 20 && poem.size() < 500);
}

TEST_CASE("Reading a poem is revealed with pips like all text") {
    std::vector<Beep> pips;
    Config config;
    config.intro = false;
    config.revealCharsPerSecond = 1250;
    config.sound = true;
    config.onBeep = [&pips](const Beep &beep) { pips.push_back(beep); };
    Pc pc(config);
    for (int i = 0; i < 100 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    pips.clear();
    pc.run("cat Downloads/private_DO_NOT_OPEN.txt");
    EXPECT(pc.sys.revealing());
    for (int i = 0; i < 300 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    EXPECT(pips.size() >= 5);
    EXPECT(pc.shows("You opened it anyway. I knew you would."));
}

TEST_SUIT_END
