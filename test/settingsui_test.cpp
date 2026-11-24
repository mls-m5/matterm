#include "mls-unit-test/unittest.h"
#include "settingsui.h"
#include "tty/screen.h"
#include <map>

namespace {
struct MemorySettings : frontend::settings::FileSystem {
    std::string text = "{}";
    std::int64_t version = 1;
    bool fail = false;
    int writes = 0;
    std::optional<frontend::settings::FileStat> stat(
        const std::string &) override {
        return frontend::settings::FileStat{version, text.size()};
    }
    bool read(const std::string &, std::string &out) override {
        out = text;
        return true;
    }
    bool writeAtomic(const std::string &,
                     const std::string &in,
                     std::string &error) override {
        if (fail) {
            error = "disk full";
            return false;
        }
        text = in;
        ++version;
        ++writes;
        return true;
    }
    void external(std::string in) {
        text = std::move(in);
        ++version;
    }
};
tty::Key key(tty::Key::Type t) {
    tty::Key k;
    k.type = t;
    return k;
}
tty::Key text(std::string s) {
    tty::Key k;
    k.type = tty::Key::Type::Text;
    k.text = std::move(s);
    return k;
}
} // namespace

TEST_SUIT_BEGIN(SettingsUI)
TEST_CASE("Shared fuzzy search finds controls; choice saves immediately; "
          "Escape only cancels current edit") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("thm");
    EXPECT(!ui.rows().empty());
    EXPECT(ui.rows().front().key == "theme");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(watch.settings().choice("theme") == "blue");
    EXPECT(fs.writes == 1);
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Escape));
    EXPECT(watch.settings().choice("theme") == "blue");
    EXPECT(fs.writes == 1);
    EXPECT(!ui.editing());
}
TEST_CASE("Committing rebases a single control onto external changes and "
          "preserves unknown members") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("scrollback");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("2000"));
    fs.external(R"({"theme":"red","future":{"a":7}})");
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 1);
    EXPECT(watch.settings().integer("scrollbackRows") == 2000);
    EXPECT(watch.settings().choice("theme") == "red");
    EXPECT(fs.text.find("\"future\"") != std::string::npos);
}
TEST_CASE(
    "An externally changed same control or broken file is never overwritten") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("theme");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    fs.external(R"({"theme":"red"})");
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 0);
    EXPECT(ui.message().find("changed") != std::string::npos);
    ui.handle(key(tty::Key::Type::Escape));
    ui.handle(key(tty::Key::Type::Enter));
    fs.external("{");
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 0);
}
TEST_CASE("Computer overrides can be added and switched back to automatic") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("Add computer");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("server"));
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down)); // auto -> grey
    ui.handle(key(tty::Key::Type::Down)); // grey -> blue
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(watch.settings().computerChoice("server") == "blue");
    ui.setQuery("server");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Home));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(watch.settings().computerChoice("server") == "auto");
}
TEST_CASE(
    "Sections, name placement and failed writes stay keyboard accessible") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.handle(key(tty::Key::Type::Tab));
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(ui.rows().front().key == "scrollbackRows");
    ui.setQuery("computerNamePlacement");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(watch.settings().choice("computerNamePlacement") == "left");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    fs.fail = true;
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(ui.editing());
    EXPECT(ui.message().find("disk full") != std::string::npos);
    EXPECT(watch.settings().choice("computerNamePlacement") == "left");
    EXPECT(fs.writes == 1);
}

TEST_CASE("Draw uses shared canvas, centers wide layouts, and handles tiny "
          "terminals") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    tty::Canvas wide(160, 40);
    ui.draw(wide);
    EXPECT(wide.at(0, 0).codePoint == U' ');
    bool title = false;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 160; ++x)
            if (wide.at(x, y).codePoint == U'm')
                title = true;
    EXPECT(title);
    tty::Canvas tiny(12, 3);
    ui.draw(tiny);
    ui.setQuery("scrollback");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("2000"));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 0);
    EXPECT(!ui.editing());
}
TEST_CASE(
    "Choices remain visible at minimum size and tiny resize suspends applies") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("computerNamePlacement");
    ui.handle(key(tty::Key::Type::Enter));
    tty::Canvas canvas(42, 14);
    ui.draw(canvas);
    std::string drawn;
    for (int y = 0; y < canvas.rows(); ++y) {
        for (int x = 0; x < canvas.columns(); ++x)
            tty::appendUtf8(drawn, canvas.at(x, y).codePoint);
        drawn += '\n';
    }
    EXPECT(drawn.find("< center >") != std::string::npos);
    tty::Canvas tiny(12, 3);
    ui.draw(tiny);
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 0);
    EXPECT(ui.editing());
    ui.handle(key(tty::Key::Type::Escape));
    EXPECT(!ui.editing());
    ui.draw(canvas);
    ui.setQuery("scrollback");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("2000"));
    ui.draw(tiny);
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 0);
    ui.draw(canvas);
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(fs.writes == 1);
}
TEST_CASE("Wide computer names retain every character in the shared screen") {
    MemorySettings fs;
    fs.text = R"({"computers":{"服务器":"blue"}})";
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("服务器");
    tty::Screen screen(100, 30);
    screen.canvas().setWideChars(true);
    screen.enter();
    ui.draw(screen.canvas());
    const auto output = screen.present();
    EXPECT(output.find("服务器") != std::string::npos);
}
TEST_SUIT_END
