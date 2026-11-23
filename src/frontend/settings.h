#pragma once

// The settings of the window (docs/tasks/settings.md): a list of definitions
// (key, text, type, default, allowed values) and the values read from
// settings.json. The document is kept whole, so that keys this version does not
// know are written back unchanged; a value that does not fit its definition is
// reported and its default is used; a document that is not JSON is an error and
// gives no settings at all.
//
// Pure: no files here (settingsfile.h), no clock (settingswatch.h), no colours
// (themes.h).
#include "jsonvalue.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frontend::settings {

// The themes by id, in the order the automatic choice uses them (after the
// first, which is today's look).
inline const std::vector<std::string> &themeIds() {
    static const std::vector<std::string> ids = {
        "grey", "blue", "green", "red", "amber"};
    return ids;
}

enum class Type { Choice, Bool, Integer, Computers };

struct Definition {
    const char *key;
    const char *label; // Short, for a list.
    const char *text;  // One sentence.
    Type type;
    std::vector<std::string> choices; // Choice: the allowed values.
    const char *defaultValue;      // As text: a choice, "true"/"false", digits;
                                   // Computers: "".
    long minimum = 0, maximum = 0; // Integer.
    bool needsRestart =
        false; // Applies to windows started afterwards (or new terminals).
};

inline const std::vector<Definition> &definitions() {
    static const std::vector<Definition> all = {
        {"theme",
         "Theme",
         "The colours of the window frame (tab strip, marks, borders) and a "
         "slight tint of the terminal background.",
         Type::Choice,
         themeIds(),
         "grey"},
        {"computers",
         "Theme per computer",
         "A theme for a computer you connect to with --daemon-host (by the "
         "name you gave it); auto picks one from its name.",
         Type::Computers,
         {},
         ""},
        {"tabActivity",
         "Tab activity",
         "What a tab shows while a hidden terminal prints on its own: a small "
         "mark, a pulse, or nothing.",
         Type::Choice,
         {"icon", "pulse", "off"},
         "icon"},
        {"windowFrame",
         "Window frame",
         "Our own frame (the tab strip is the title bar) or the desktop's.",
         Type::Choice,
         {"own", "system"},
         "own",
         0,
         0,
         true},
        {"scrollbackRows",
         "Scrollback rows",
         "How many lines each terminal keeps above the screen.",
         Type::Integer,
         {},
         "1000",
         100,
         100000,
         true},
        {"computerNamePlacement",
         "Computer name in the tab strip",
         "Where the name of another computer is written in the strip's gap: in "
         "the middle, next to the tabs, or against the buttons.",
         Type::Choice,
         {"center", "left", "right"},
         "center"},
        {"confirmClose",
         "Confirm closing a busy pane",
         "Ask before closing a pane that runs a program other than the shell.",
         Type::Bool,
         {},
         "true"},
    };
    return all;
}

inline const Definition *definition(std::string_view key) {
    for (const auto &d : definitions()) {
        if (key == d.key) {
            return &d;
        }
    }
    return nullptr;
}

inline bool validComputerName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == ' ' ||
        name.back() == ' ') {
        return false;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

namespace detail {
inline bool allowedTheme(const std::string &id) {
    return std::find(themeIds().begin(), themeIds().end(), id) !=
           themeIds().end();
}
inline std::string join(const std::vector<std::string> &items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += (i ? ", " : "") + items[i];
    }
    return out;
}
inline bool parseInteger(const std::string &text, long &out) {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    long n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + (c - '0');
    }
    out = n;
    return true;
}
} // namespace detail

class Settings;
struct LoadResult;

class Settings {
public:
    Settings()
        : doc_(json::Value::object()) {}

    static LoadResult fromText(std::string_view text);

    std::string choice(const std::string &key) const {
        const Definition *d = definition(key);
        const json::Value *v = doc_.find(key);
        if (d && d->type == Type::Choice && v) {
            if (const auto s = v->string()) {
                if (std::find(d->choices.begin(), d->choices.end(), *s) !=
                    d->choices.end()) {
                    return *s;
                }
            }
        }
        return d ? d->defaultValue : std::string();
    }
    long integer(const std::string &key) const {
        const Definition *d = definition(key);
        long fallback = 0;
        if (d) {
            detail::parseInteger(d->defaultValue, fallback);
        }
        const json::Value *v = doc_.find(key);
        if (d && d->type == Type::Integer && v) {
            if (const auto n = v->integer()) {
                if (*n >= d->minimum && *n <= d->maximum) {
                    return long(*n);
                }
            }
        }
        return fallback;
    }
    bool flag(const std::string &key) const {
        const Definition *d = definition(key);
        const bool fallback = d && std::string(d->defaultValue) == "true";
        const json::Value *v = doc_.find(key);
        if (d && d->type == Type::Bool && v) {
            if (const auto b = v->boolean()) {
                return *b;
            }
        }
        return fallback;
    }
    // "auto" or a theme id.
    std::string computerChoice(const std::string &name) const {
        const json::Value *computers = doc_.find("computers");
        const json::Value *v = computers && computers->isObject()
                                   ? computers->find(name)
                                   : nullptr;
        if (v) {
            if (const auto s = v->string()) {
                if (*s == "auto" || detail::allowedTheme(*s)) {
                    return *s;
                }
            }
        }
        return "auto";
    }
    // Names that have an entry (valid or not), for a list.
    std::vector<std::string> computerNames() const {
        std::vector<std::string> names;
        const json::Value *computers = doc_.find("computers");
        if (computers && computers->isObject()) {
            for (const auto &m : computers->members()) {
                names.push_back(m.first);
            }
        }
        return names;
    }

    // Changes a value, given as text ("blue", "2000",
    // "on"/"off"/"true"/"false"). False with `error` when it does not fit.
    bool set(const std::string &key,
             const std::string &value,
             std::string &error) {
        const Definition *d = definition(key);
        if (!d) {
            error = "there is no setting called " + key;
            return false;
        }
        switch (d->type) {
        case Type::Choice:
            if (std::find(d->choices.begin(), d->choices.end(), value) ==
                d->choices.end()) {
                error = std::string(d->label) + ": " + value +
                        " is not one of " + detail::join(d->choices);
                return false;
            }
            doc_.set(key, json::Value::text(value));
            return true;
        case Type::Bool: {
            const bool on = value == "on" || value == "true" || value == "yes";
            const bool off =
                value == "off" || value == "false" || value == "no";
            if (!on && !off) {
                error =
                    std::string(d->label) + ": " + value + " is not on or off";
                return false;
            }
            doc_.set(key, json::Value::boolean(on));
            return true;
        }
        case Type::Integer: {
            long n = 0;
            if (!detail::parseInteger(value, n) || n < d->minimum ||
                n > d->maximum) {
                error = std::string(d->label) + ": " + value +
                        " is not a whole number from " +
                        std::to_string(d->minimum) + " to " +
                        std::to_string(d->maximum);
                return false;
            }
            doc_.set(key, json::Value::number(n));
            return true;
        }
        case Type::Computers:
            error =
                std::string(d->label) + " is set per computer (setComputer)";
            return false;
        }
        return false;
    }
    bool setComputer(const std::string &name,
                     const std::string &theme,
                     std::string &error) {
        if (!validComputerName(name)) {
            error = "that is not a computer name";
            return false;
        }
        if (theme != "auto" && !detail::allowedTheme(theme)) {
            error =
                theme + " is not auto or one of " + detail::join(themeIds());
            return false;
        }
        json::Value *computers = doc_.find("computers");
        if (!computers || !computers->isObject()) {
            doc_.set("computers", json::Value::object());
            computers = doc_.find("computers");
        }
        computers->set(name, json::Value::text(theme));
        return true;
    }

    // Rewrite the whole file in definition order, including effective defaults.
    // Unknown top-level members follow the known settings in their original
    // order. Build a new document so saving never mutates the loaded state.
    std::string toText() const {
        auto out = json::Value::object();
        for (const auto &d : definitions()) {
            switch (d.type) {
            case Type::Choice:
                out.set(d.key, json::Value::text(choice(d.key)));
                break;
            case Type::Bool:
                out.set(d.key, json::Value::boolean(flag(d.key)));
                break;
            case Type::Integer:
                out.set(d.key, json::Value::number(integer(d.key)));
                break;
            case Type::Computers: {
                auto computers = json::Value::object();
                for (const auto &name : computerNames())
                    computers.set(name,
                                  json::Value::text(computerChoice(name)));
                out.set(d.key, std::move(computers));
                break;
            }
            }
        }
        for (const auto &member : doc_.members()) {
            if (!definition(member.first))
                out.set(member.first, member.second);
        }
        return json::dump(out, true);
    }

    // The values that matter are equal (what else the file holds does not
    // count).
    bool sameValues(const Settings &other) const {
        for (const auto &d : definitions()) {
            switch (d.type) {
            case Type::Choice:
                if (choice(d.key) != other.choice(d.key)) {
                    return false;
                }
                break;
            case Type::Integer:
                if (integer(d.key) != other.integer(d.key)) {
                    return false;
                }
                break;
            case Type::Bool:
                if (flag(d.key) != other.flag(d.key)) {
                    return false;
                }
                break;
            case Type::Computers: {
                std::set<std::string> names;
                for (const auto &n : computerNames()) {
                    names.insert(n);
                }
                for (const auto &n : other.computerNames()) {
                    names.insert(n);
                }
                for (const auto &n : names) {
                    if (computerChoice(n) != other.computerChoice(n)) {
                        return false;
                    }
                }
                break;
            }
            }
        }
        return true;
    }

private:
    friend struct LoadResult;
    json::Value doc_;
};

struct LoadResult {
    Settings settings;
    std::vector<std::string>
        warnings; // Values that did not fit (the default is used for them).
    std::string error; // Set: the file is no settings document; `settings` is
                       // the defaults.
    bool ok() const {
        return error.empty();
    }
};

inline LoadResult Settings::fromText(std::string_view text) {
    LoadResult result;
    std::string why;
    auto doc = json::parse(text, &why);
    if (!doc) {
        result.error = why;
        return result;
    }
    if (!doc->isObject()) {
        result.error = "the settings file must hold a JSON object";
        return result;
    }
    result.settings.doc_ = std::move(*doc);
    const Settings &s = result.settings;
    for (const auto &d : definitions()) {
        const json::Value *v = s.doc_.find(d.key);
        if (!v) {
            continue;
        }
        const std::string key = d.key;
        switch (d.type) {
        case Type::Choice: {
            const auto given = v->string();
            if (!given ||
                std::find(d.choices.begin(), d.choices.end(), *given) ==
                    d.choices.end()) {
                result.warnings.push_back(
                    key + ": " +
                    (given ? "\"" + *given + "\"" : json::dump(*v)) +
                    " is not one of " + detail::join(d.choices) + "; using " +
                    d.defaultValue);
            }
            break;
        }
        case Type::Bool:
            if (!v->boolean()) {
                result.warnings.push_back(key + ": " + json::dump(*v) +
                                          " is not true or false; using " +
                                          d.defaultValue);
            }
            break;
        case Type::Integer: {
            const auto n = v->integer();
            if (!n || *n < d.minimum || *n > d.maximum) {
                result.warnings.push_back(key + ": " + json::dump(*v) +
                                          " is not a whole number from " +
                                          std::to_string(d.minimum) + " to " +
                                          std::to_string(d.maximum) +
                                          "; using " + d.defaultValue);
            }
            break;
        }
        case Type::Computers:
            if (!v->isObject()) {
                result.warnings.push_back(
                    key +
                    ": must be an object of computer name to theme; ignored");
                break;
            }
            for (const auto &m : v->members()) {
                const auto theme = m.second.string();
                if (!theme ||
                    (*theme != "auto" && !detail::allowedTheme(*theme))) {
                    result.warnings.push_back(
                        key + "." + m.first + ": " + json::dump(m.second) +
                        " is not auto or one of " + detail::join(themeIds()) +
                        "; using auto");
                }
            }
            break;
        }
    }
    return result;
}

} // namespace frontend::settings
