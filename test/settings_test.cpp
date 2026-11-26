#include "mls-unit-test/unittest.h"
#include "settings.h"

using namespace frontend::settings;

TEST_SUIT_BEGIN(Settings)

TEST_CASE("The definitions: unique keys, a default that is allowed, a text for "
          "each") {
    std::set<std::string> keys;
    for (const auto &d : definitions()) {
        EXPECT(keys.insert(d.key).second);
        EXPECT(std::string(d.label).size() > 0 &&
               std::string(d.text).size() > 0);
        Settings fresh;
        if (d.type == Type::Choice) {
            EXPECT(std::find(d.choices.begin(),
                             d.choices.end(),
                             std::string(d.defaultValue)) != d.choices.end());
            EXPECT_EQ(fresh.choice(d.key), std::string(d.defaultValue));
        }
    }
    EXPECT(keys.count("theme") && keys.count("computers") &&
           keys.count("tabActivity") && keys.count("windowFrame") &&
           keys.count("scrollbackRows") && keys.count("confirmClose") &&
           keys.count("computerNamePlacement"));
}

TEST_CASE(
    "Saved SSH servers are separate from theme names and reject options") {
    Settings s;
    std::string error;
    EXPECT(s.serverHosts().empty());
    EXPECT(!s.addServerHost("-oProxyCommand=bad", error));
    EXPECT(!s.addServerHost("user@host;bad", error));
    EXPECT(s.addServerHost("user@server", error));
    EXPECT(s.addServerHost("user@server", error));
    EXPECT_EQ(s.serverHosts().size(), std::size_t(1));
    EXPECT(s.computerNames().empty());
    EXPECT(!Settings().sameValues(s));
    auto loaded = Settings::fromText(s.toText());
    EXPECT(loaded.ok());
    EXPECT_EQ(loaded.settings.serverHosts().front(),
              std::string("user@server"));
    EXPECT(loaded.settings.replaceServerHost("user@server", "other", error));
    EXPECT_EQ(loaded.settings.serverHosts().front(), std::string("other"));
    EXPECT(loaded.settings.removeServerHost("other", error));
    EXPECT(loaded.settings.serverHosts().empty());
    EXPECT(Settings::fromText(R"({"servers":["-bad","ok","ok",3]})")
               .settings.serverHosts() == std::vector<std::string>{"ok"});
}

TEST_CASE("Cell vertical alignment defaults to top and saves bottom") {
    Settings settings;
    EXPECT_EQ(settings.choice("cellVerticalAlignment"), std::string("top"));
    std::string error;
    EXPECT(settings.set("cellVerticalAlignment", "bottom", error));
    const auto loaded = Settings::fromText(settings.toText());
    EXPECT(loaded.ok());
    EXPECT_EQ(loaded.settings.choice("cellVerticalAlignment"),
              std::string("bottom"));
}

TEST_CASE("Joining full TUI lines is configurable and on by default") {
    Settings settings;
    EXPECT(settings.flag("copyJoinFullTuiRows"));
    std::string error;
    EXPECT(settings.set("copyJoinFullTuiRows", "false", error));
    const auto loaded = Settings::fromText(settings.toText());
    EXPECT(loaded.ok() && !loaded.settings.flag("copyJoinFullTuiRows"));
}

TEST_CASE("Draw batching is a default-on client bool and persists off") {
    auto settings = frontend::settings::Settings{};
    EXPECT(settings.flag("drawBatching"));
    std::string error;
    EXPECT(settings.set("drawBatching", "false", error));
    auto loaded = Settings::fromText(settings.toText());
    EXPECT(loaded.ok() && !loaded.settings.flag("drawBatching"));
}

TEST_CASE("No file: the defaults") {
    Settings s;
    EXPECT_EQ(s.choice("theme"), std::string("grey"));
    EXPECT_EQ(s.choice("tabActivity"), std::string("icon"));
    EXPECT_EQ(s.choice("windowFrame"), std::string("own"));
    EXPECT_EQ(s.integer("scrollbackRows"), 1000);
    EXPECT(s.flag("confirmClose"));
    EXPECT_EQ(s.choice("computerNamePlacement"), std::string("center"));
    EXPECT_EQ(s.computerChoice("server"), std::string("auto"));
}

TEST_CASE("A file sets what it names; the rest stays default; nothing to warn "
          "about") {
    const auto loaded = Settings::fromText(
        R"({"theme":"blue","tabActivity":"off","scrollbackRows":5000,"confirmClose":false,"computers":{"server":"green","box":"auto"}})");
    EXPECT(loaded.ok());
    EXPECT(loaded.warnings.empty());
    const auto &s = loaded.settings;
    EXPECT_EQ(s.choice("theme"), std::string("blue"));
    EXPECT_EQ(s.choice("tabActivity"), std::string("off"));
    EXPECT_EQ(s.choice("windowFrame"), std::string("own"));
    EXPECT_EQ(s.integer("scrollbackRows"), 5000);
    EXPECT(!s.flag("confirmClose"));
    EXPECT_EQ(s.computerChoice("server"), std::string("green"));
    EXPECT_EQ(s.computerChoice("box"), std::string("auto"));
    EXPECT_EQ(s.computerChoice("elsewhere"), std::string("auto"));
}

TEST_CASE("A value that does not fit is reported and the default is used; the "
          "rest still applies") {
    const auto loaded = Settings::fromText(
        R"({"theme":"purple","tabActivity":3,"scrollbackRows":5,"confirmClose":"yes","windowFrame":"system","computers":{"a":"nope","b":"red"}})");
    EXPECT(loaded.ok());
    EXPECT_EQ(loaded.warnings.size(), std::size_t(5));
    const auto &s = loaded.settings;
    EXPECT_EQ(s.choice("theme"), std::string("grey"));
    EXPECT_EQ(s.choice("tabActivity"), std::string("icon"));
    EXPECT_EQ(s.integer("scrollbackRows"), 1000);
    EXPECT(s.flag("confirmClose"));
    EXPECT_EQ(s.choice("windowFrame"), std::string("system"));
    EXPECT_EQ(s.computerChoice("a"), std::string("auto"));
    EXPECT_EQ(s.computerChoice("b"), std::string("red"));
    bool namesTheme = false;
    for (const auto &w : loaded.warnings) {
        namesTheme = namesTheme || (w.find("theme") != std::string::npos &&
                                    w.find("purple") != std::string::npos);
    }
    EXPECT(namesTheme);
}

TEST_CASE("A broken file is an error, there are no settings from it") {
    for (const char *bad : {"",
                            "{",
                            "[1]",
                            "\"x\"",
                            "{\"theme\":}",
                            "{\"theme\":\"grey\",\"theme\":\"blue\"}",
                            "null"}) {
        const auto loaded = Settings::fromText(bad);
        EXPECT(!loaded.ok());
        EXPECT(!loaded.error.empty());
    }
}

TEST_CASE("Unknown keys and values follow all known settings when saved") {
    const std::string text =
        R"({"future":{"a":[1,2.5,null]},"theme":"red","note":"keep me"})";
    auto loaded = Settings::fromText(text);
    EXPECT(loaded.ok());
    std::string error;
    EXPECT(loaded.settings.set("theme", "green", error));
    EXPECT(loaded.settings.set("tabActivity", "pulse", error));
    const auto out = loaded.settings.toText();
    const auto again = Settings::fromText(out);
    EXPECT(again.ok());
    EXPECT_EQ(again.settings.choice("theme"), std::string("green"));
    EXPECT_EQ(again.settings.choice("tabActivity"), std::string("pulse"));
    EXPECT(out.find("\"future\"") != std::string::npos &&
           out.find("2.5") != std::string::npos);
    EXPECT(out.find("keep me") != std::string::npos);
    EXPECT(out.find("\"confirmClose\"") < out.find("\"future\""));
    EXPECT(out.find("\"future\"") < out.find("\"note\""));
    EXPECT(out.find("\"theme\"") < out.find("\"tabActivity\""));
}

TEST_CASE(
    "Saving writes every default with stable formatting in definition order") {
    const std::string expected = "{\n"
                                 "  \"theme\": \"grey\",\n"
                                 "  \"computers\": {},\n"
                                 "  \"tabActivity\": \"icon\",\n"
                                 "  \"windowFrame\": \"own\",\n"
                                 "  \"scrollbackRows\": 1000,\n"
                                 "  \"cellVerticalAlignment\": \"top\",\n"
                                 "  \"computerNamePlacement\": \"center\",\n"
                                 "  \"confirmClose\": true,\n"
                                 "  \"copyJoinFullTuiRows\": true,\n"
                                 "  \"drawBatching\": true,\n"
                                 "  \"terminalListPreview\": true\n"
                                 "}\n";
    EXPECT_EQ(Settings().toText(), expected);
    EXPECT_EQ(Settings::fromText(expected).settings.toText(), expected);
    EXPECT_EQ(Settings::fromText(R"({"confirmClose":true,"theme":"grey"})")
                  .settings.toText(),
              expected);
}

TEST_CASE("Saving writes effective values and retains nested unknown values "
          "after known ones") {
    auto loaded = Settings::fromText(
        R"({"future":{"nested":[null,1.20,"x"]},"theme":"invalid","scrollbackRows":5,"confirmClose":"yes","computers":{"srv":"invalid","box":"green"}})");
    EXPECT(loaded.ok());
    const auto text = loaded.settings.toText();
    const auto again = Settings::fromText(text);
    EXPECT(again.ok());
    EXPECT(again.warnings.empty());
    EXPECT(loaded.settings.sameValues(again.settings));
    EXPECT_EQ(text, again.settings.toText());
    EXPECT(text.find("1.20") != std::string::npos);
    EXPECT(text.find("\"confirmClose\"") < text.find("\"future\""));
    EXPECT(text.find("\"srv\": \"auto\"") != std::string::npos);
}

TEST_CASE(
    "The place of the computer's name is a choice: centre, left or right") {
    Settings s;
    std::string error;
    EXPECT(s.set("computerNamePlacement", "left", error));
    EXPECT_EQ(s.choice("computerNamePlacement"), std::string("left"));
    EXPECT(!s.set("computerNamePlacement", "top", error));
    EXPECT(error.find("center, left, right") != std::string::npos);
    const auto loaded =
        Settings::fromText(R"({"computerNamePlacement":"middle"})");
    EXPECT_EQ(loaded.warnings.size(), std::size_t(1));
    EXPECT_EQ(loaded.settings.choice("computerNamePlacement"),
              std::string("center"));
}

TEST_CASE("set checks the value against the definition") {
    Settings s;
    std::string error;
    EXPECT(!s.set("theme", "purple", error));
    EXPECT(error.find("grey") != std::string::npos); // Names what is allowed.
    EXPECT(!s.set("nosuchkey", "x", error));
    EXPECT(!s.set("scrollbackRows", "5", error));
    EXPECT(!s.set("scrollbackRows", "many", error));
    EXPECT(s.set("scrollbackRows", "2000", error));
    EXPECT_EQ(s.integer("scrollbackRows"), 2000);
    EXPECT(!s.set("confirmClose", "maybe", error));
    EXPECT(s.set("confirmClose", "off", error));
    EXPECT(!s.flag("confirmClose"));
    EXPECT(s.set("confirmClose", "on", error));
    EXPECT(s.flag("confirmClose"));
    EXPECT_EQ(s.choice("theme"),
              std::string("grey")); // The failed ones changed nothing.
}

TEST_CASE("The theme of a computer: set, back to automatic, and the name must "
          "be sane") {
    Settings s;
    std::string error;
    EXPECT(s.setComputer("server", "green", error));
    EXPECT_EQ(s.computerChoice("server"), std::string("green"));
    EXPECT(s.setComputer("server", "auto", error));
    EXPECT_EQ(s.computerChoice("server"), std::string("auto"));
    EXPECT(!s.setComputer("server", "plaid", error));
    EXPECT(!s.setComputer("", "red", error));
    EXPECT(!s.setComputer(std::string(300, 'x'), "red", error));
    EXPECT(!s.setComputer("bad\nname", "red", error));
    // A round trip keeps it.
    EXPECT(s.setComputer("box", "blue", error));
    const auto again = Settings::fromText(s.toText());
    EXPECT(again.ok());
    EXPECT_EQ(again.settings.computerChoice("box"), std::string("blue"));
}

TEST_CASE("Equal settings compare equal, whatever else is in the file") {
    const auto a = Settings::fromText(R"({"theme":"blue","other":1})");
    const auto b = Settings::fromText(R"({"other":2,"theme":"blue"})");
    const auto c = Settings::fromText(R"({"theme":"green"})");
    EXPECT(a.settings.sameValues(b.settings));
    EXPECT(!a.settings.sameValues(c.settings));
    EXPECT(Settings().sameValues(Settings::fromText("{}").settings));
}

TEST_CASE(
    "Terminal list preview uses the existing bool model and defaults on") {
    Settings settings;
    EXPECT(settings.flag("terminalListPreview"));
    std::string error;
    EXPECT(settings.set("terminalListPreview", "false", error));
    const auto again = Settings::fromText(settings.toText());
    EXPECT(again.ok() && !again.settings.flag("terminalListPreview"));
    EXPECT(again.settings.toText() == settings.toText());
}
TEST_SUIT_END
