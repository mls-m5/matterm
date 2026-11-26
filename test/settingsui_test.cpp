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
TEST_CASE("Servers can be added, edited and removed in local Settings") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("Add server");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("user@host"));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT_EQ(watch.settings().serverHosts().front(), std::string("user@host"));
    ui.setQuery("user@host");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(text("other"));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT_EQ(watch.settings().serverHosts().front(), std::string("other"));
    ui.setQuery("other");
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Backspace));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(watch.settings().serverHosts().empty());
    EXPECT(fs.writes == 3);
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
TEST_CASE("Mouse uses centered drawn controls and only explicit left presses "
          "apply or cancel") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    tty::Canvas canvas(120, 40);
    ui.draw(canvas);
    const auto click =
        [&](int x, int y, bool press = true, bool motion = false) {
            auto k = key(tty::Key::Type::Mouse);
            k.column = x;
            k.row = y;
            k.press = press;
            k.motion = motion;
            ui.handle(k);
        };
    click(35, 10); // Centered first setting row: Theme.
    EXPECT(ui.editing());
    ui.handle(key(tty::Key::Type::Down));
    ui.draw(canvas);
    click(90, 29); // Outside explicit action buttons: must not save.
    click(34, 30, false);
    click(34, 30, true, true);
    EXPECT(fs.writes == 0);
    click(34, 30); // Apply.
    EXPECT(fs.writes == 1);
    EXPECT(watch.settings().choice("theme") == "blue");
    ui.draw(canvas);
    click(35, 10);
    ui.handle(key(tty::Key::Type::Down));
    ui.draw(canvas);
    click(45, 30); // Cancel.
    EXPECT(!ui.editing());
    EXPECT(fs.writes == 1);
}

TEST_CASE("Mouse follows narrow scroll and resize layout, sections and search "
          "focus") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    tty::Canvas narrow(42, 14);
    const auto click = [&](int x, int y, int button = 0) {
        auto k = key(tty::Key::Type::Mouse);
        k.column = x;
        k.row = y;
        k.button = button;
        ui.handle(k);
    };
    ui.draw(narrow);
    click(3, 8); // Terminal section.
    EXPECT(ui.rows().front().key == "scrollbackRows");
    ui.draw(narrow);
    click(3, 3); // Search focuses rows rather than sections.
    ui.handle(text("preview"));
    EXPECT(ui.rows().size() == 1);
    ui.draw(narrow);
    click(20, 6, 2);
    EXPECT(!ui.editing());
    click(20, 6);
    EXPECT(ui.editing());
    ui.handle(key(tty::Key::Type::Escape));
    ui.setQuery("");
    ui.handle(key(tty::Key::Type::End));
    const auto last = ui.rows().back().value;
    ui.draw(narrow); // Only one visible row, scrolled to last.
    click(20, 6);
    EXPECT(ui.editing());
    tty::Canvas tiny(30, 8);
    ui.draw(tiny);
    click(19, 8);
    EXPECT(fs.writes == 0);
    tty::Canvas wide(120, 40);
    ui.draw(wide);
    click(45, 30); // Resized Cancel.
    EXPECT(!ui.editing());
    EXPECT(fs.writes == 0);
    EXPECT(ui.rows().back().value == last);
}

TEST_CASE("Draw batching is in Terminal and searchable as a live bool") {
    MemorySettings fs;
    frontend::settings::SettingsWatcher watch(
        fs, "settings.json", [] { return 0; });
    frontend::settings::SettingsUI ui(watch);
    ui.setQuery("Draw batching");
    EXPECT(ui.rows().size() == 1);
    EXPECT(ui.rows().front().key == "drawBatching");
    EXPECT(ui.rows().front().section == 1);
    ui.handle(key(tty::Key::Type::Enter));
    ui.handle(key(tty::Key::Type::Down));
    ui.handle(key(tty::Key::Type::Enter));
    EXPECT(!watch.settings().flag("drawBatching"));
}

TEST_SUIT_END
